#include <ATen/Functions.h>
#include <ATen/cuda/CUDAContext.h>
#include <ATen/cuda/CUDAGeneratorImpl.h>
#include <ATen/cuda/CUDAGraph.h>
#include <ATen/cuda/Exceptions.h>
#include <ATen/cuda/nvrtc_stub/ATenNVRTC.h>
#include <c10/core/CPUAllocator.h>
#include <c10/cuda/CUDACachingAllocator.h>
#include <c10/cuda/CUDAFunctions.h>
#include <c10/cuda/CUDAGuard.h>
#include <c10/cuda/driver_api.h>
// Not good, aten shouldn't depend upon torch...
#include <torch/csrc/cuda/CUDAPluggableAllocator.h>

#include <cstddef>
#include <functional>

#include <dlfcn.h>

#include "nv_decode.h"  // Assuming this header defines __cu_demangle.

#include <GetTypeInformation.h>



namespace at::cuda {

static bool _cuda_graphs_debug = false;

constexpr size_t TWO_MIB = 2 * 1024 * 1024; // 2 MiB in bytes

size_t roundUpToNearestTwoMiB(size_t size) {
  return ((size + TWO_MIB - 1) / TWO_MIB) * TWO_MIB;
}

bool regions_intersect(void *ptr1, size_t size1,
                       void *ptr2, size_t size2)
{
    if (size1 == 0 || size2 == 0) {
        return false;
    }

    uintptr_t a_start = (uintptr_t)ptr1;
    uintptr_t a_end   = a_start + size1;
    uintptr_t b_start = (uintptr_t)ptr2;
    uintptr_t b_end   = b_start + size2;

    // they intersect iff each region’s start is before the other’s end
    return (a_start < b_end) && (b_start < a_end);
}


// To support stream capture across multiple threads, we use a global
// hashmap mapping cuda stream capture IDs to CUDAGraph objects. This
// was originally a thread_local std::stack<CUDAGraph*>, but that was
// not acceptable since stream capture does span threads in certain
// circumstances (in particular, during autograd).
static std::mutex _currently_capturing_graphs_mutex;
static ska::flat_hash_map<CaptureId_t, CUDAGraph*> _currently_capturing_graphs;

void *align_to_8_bytes(void *ptr) {
    uintptr_t p = (uintptr_t)ptr;
    // Add 7 and clear the lower 3 bits to round up to a multiple of 8
    uintptr_t aligned = (p + (uintptr_t)7) & ~((uintptr_t)7);
    return (void *)aligned;
}


std::optional<std::tuple<size_t, size_t>>
checkAllocationWithinGraph(void* ptr, const std::vector<DynamicGraphAllocation>& sorted_allocations) {
  // since allocations is sorted in ptr order, we can search it in log time

  // finds first allocation base address is greater than ptr. That seems pretty wrong to me...
  auto it = std::upper_bound(sorted_allocations.begin(), sorted_allocations.end(), ptr, [](const void* p, const DynamicGraphAllocation& alloc) {
    return p < alloc.ptr;
  });
  // upper_bound finds the first allocation whose ptr is strictly greater than our search
  if (it == sorted_allocations.begin()) {
    return std::nullopt; // the ptr is before our first allocation
  }
  // so we must decrement to get to the one we actually want
  // okay, that works. Or does it? What if my allocation is not within one of my alloations?
  --it;
  // something seems wrong here...
  void* begin = it->ptr;
  void* end = (char*) begin + it->size;
  if (ptr >= begin && ptr < end) {
    size_t offset = (char*) ptr - (char*) begin;
    return std::make_tuple(it->alloc_idx, offset);
  }
  return std::nullopt;
}

  std::vector<std::tuple<size_t, size_t, size_t>> gather_pointers_in_node(
    const std::variant<BasicType, StructType, ArrayType>& type_info,
    char *arg_buffer,
    const std::vector<DynamicGraphAllocation>& sorted_allocations,
    size_t global_offset_bytes) {
  std::vector<std::tuple<size_t, size_t, size_t>> results;
  std::visit([&results, global_offset_bytes, &sorted_allocations, &arg_buffer](auto&& arg) {
    using T = std::decay_t<decltype(arg)>;
    if constexpr (std::is_same_v<T, BasicType>) {
      size_t offset = global_offset_bytes + arg.offset;
      if (arg.is_pointer) {
        TORCH_INTERNAL_ASSERT(offset % 8 == 0, "All pointers should be 8 byte aligned.");
        if (auto result = checkAllocationWithinGraph(*((void**)(arg_buffer + offset)), sorted_allocations)) {
          results.push_back({std::get<0>(*result), std::get<1>(*result), offset});
        }
      }
    } else if constexpr (std::is_same_v<T, StructType>) {
      size_t base_offset = global_offset_bytes + arg.offset;
      for (auto&& [_, member]: arg.members) {
        auto &&recursive_results = gather_pointers_in_node(member, arg_buffer, sorted_allocations, base_offset);
        results.insert(results.end(), recursive_results.begin(), recursive_results.end());
      }
    } else if constexpr (std::is_same_v<T, ArrayType>) {
      size_t element_size = std::visit([](auto&& element) -> size_t {
        using ElementT = std::decay_t<decltype(element)>;
        if constexpr (std::is_same_v<ElementT, BasicType>) {
          return element.size;
        } else if constexpr (std::is_same_v<ElementT, StructType>) {
          return element.size;
        }
        return 0; // Should not happen
      }, arg.element_type);

      size_t base_offset = global_offset_bytes;

      for (size_t i = 0; i < arg.num_elements; ++i) {
        auto &&up_cast = std::visit(conversion_visitor, arg.element_type);
        auto &&recursive_results = gather_pointers_in_node(up_cast, arg_buffer, sorted_allocations, base_offset);
        results.insert(results.end(), recursive_results.begin(), recursive_results.end());
        base_offset += element_size;
      }
    }
  }, type_info);
  return results;
}



std::string demangle(const std::string &mangled) {
    size_t length = 0;
    int status = 0;
    char* output_buffer = nullptr;
    
    // Call the demangling function.
    char* demangled = __cu_demangle(mangled.c_str(), output_buffer, &length, &status);
    
    // Check if demangling was successful.
    if (status != 0 || demangled == nullptr) {
        // In case of error, return the original mangled string.
        return mangled;
    }
    
    // Convert the demangled C-string into a std::string.
    std::string result(demangled);
    
    // Free the memory allocated by __cu_demangle.
    free(demangled);
    
    return result;
}


MempoolId_t graph_pool_handle() {
  // Sets just the second value, to distinguish it from MempoolId_ts created from
  // cudaStreamGetCaptureInfo id_s in capture_begin.
  return c10::cuda::MemPool::graph_pool_handle();
}

/**
 * Note [CUDA Graph Wrapper Class]
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 * Q: Why do we need graph capture and launch bindings in Pytorch?
 *    Why can't they live in a user extension, for example?
 *
 * A1: Convenience.
 * A2: To ensure valid numerics on replay, some native CUDA ops (like RNG ops with
 *     CPU statefulness) need cooperation from the capture and replay bindings
 *     (see Note [CUDA Graph-safe RNG states] in CUDAGeneratorImpl.h).
 *
 *     We can't expect users to know about this cooperation.  If users write capture
 *     bindings naively in an extension, they likely won't interact with the native
 *     ops properly.  Their graphs would yield invalid numerics on replay.
 */

/**
 * Note [Interaction with CUDA graph capture] in CUDACachingAllocator.cpp
 * describes memory management for captures.
 */

CUDAGraph::CUDAGraph(bool keep_graph)
  // CUDAStreams may not be default-constructed.
  : capture_stream_(at::cuda::getCurrentCUDAStream()),
    keep_graph_(keep_graph) {
}

void CUDAGraph::register_generator_state(
    c10::intrusive_ptr<at::CUDAGeneratorState> state) {
  captured_generator_states_[std::move(state)] = 0;
}

void CUDAGraph::register_generator_state(const at::Generator& generator) {
  c10::intrusive_ptr<CUDAGeneratorImpl> cuda_gen =
      dynamic_intrusive_pointer_cast<CUDAGeneratorImpl>(
          generator.getIntrusivePtr());
  cuda_gen->register_graph(this);
}

void CUDAGraph::capture_begin(MempoolId_t pool/*=0*/, cudaStreamCaptureMode capture_mode, bool dynamic_graph) {
  TORCH_CHECK(!has_graph_exec_,
              "This CUDAGraph instance already owns a captured graph. "
              "To capture a new graph, create a new instance.");

  // default generator is always registered
  auto* gen = get_generator_or_default<CUDAGeneratorImpl>(
      std::nullopt, cuda::detail::getDefaultCUDAGenerator());
  gen->register_graph(this);

  for (auto& [generator_state, wholegraph_increments] :
       captured_generator_states_) {
    generator_state->capture_prologue();
  }

  auto stream = at::cuda::getCurrentCUDAStream();

  TORCH_CHECK(stream != at::cuda::getDefaultCUDAStream(),
              "CUDA graphs must be captured on a non-default stream. "
              "(However, after capture, it's ok to replay them on the "
              "default stream.)");

  capture_stream_ = stream;
  capture_dev_ = c10::cuda::current_device();

  if (pool.first != 0 || pool.second != 0) {
    // Either value being nonzero means the user supplied a pool to share.
    // But only one should be nonzero.
    // If pool was created by another graph's capture_begin, first should be nonzero.
    // If pool was created by graph_pool_handle, second should be nonzero.
    TORCH_INTERNAL_ASSERT(!(pool.first && pool.second));
    mempool_id_ = pool;
  } else {
    // User did not ask us to share a mempool. Create graph pool handle using is_user_created=false.
    // Sets just the first value, to distinguish it from MempoolId_ts created by graph_pool_handle().
    mempool_id_ = c10::cuda::MemPool::graph_pool_handle(false);
    TORCH_INTERNAL_ASSERT(mempool_id_.first > 0);
  }

  // I'm super confused...
  // Addendum: beginAllocateStreamToPool is now called before cudaStreamBeginCapture to prevent an
  // autograd thread's free() call triggering an invalid cudaEventRecord in the caching allocator
  // due to the capture status being updated _after_ a capture had already started.
  c10::cuda::CUDACachingAllocator::beginAllocateToPool(capture_dev_, mempool_id_, [this](cudaStream_t stream) {
      cudaStreamCaptureStatus status{};
      CaptureId_t stream_capture_id = 0;
      AT_CUDA_CHECK(cudaStreamGetCaptureInfo(stream, &status, &stream_capture_id));
      return status == cudaStreamCaptureStatus::cudaStreamCaptureStatusActive && stream_capture_id == capture_id_;
  });

  dynamic_graph_ = dynamic_graph;

  // cudaStreamCaptureModeGlobal is the most conservative option to
  // prevent potentially unsafe CUDA API calls during capture.  See
  // https://docs.nvidia.com/cuda/cuda-runtime-api/group__CUDART__STREAM.html#group__CUDART__STREAM_1g9d0535d93a214cbf126835257b16ba85

  // std::cout << "GALVEZ: cudaStreamBeginCapture" << std::endl;
  AT_CUDA_CHECK(cudaStreamBeginCapture(capture_stream_, capture_mode));

  cudaStreamCaptureStatus status{};
  AT_CUDA_CHECK(cudaStreamGetCaptureInfo(stream, &status, &capture_id_));
  TORCH_INTERNAL_ASSERT(status == cudaStreamCaptureStatus::cudaStreamCaptureStatusActive);

  {
    std::unique_lock<std::mutex> lock(_currently_capturing_graphs_mutex);
    _currently_capturing_graphs.emplace(capture_id_, this);
  }
}

void CUDAGraph::capture_end() {
  auto stream = at::cuda::getCurrentCUDAStream();

  TORCH_CHECK(stream == capture_stream_,
              "Capture must end on the same stream it began on.");

  AT_CUDA_CHECK(cudaStreamEndCapture(capture_stream_, &graph_));

  c10::cuda::CUDACachingAllocator::endAllocateToPool(capture_dev_, mempool_id_);

  TORCH_CHECK(graph_ != nullptr, "Invalid capture.");

  for (auto& [generator_state, wholegraph_increments] :
       captured_generator_states_) {
    wholegraph_increments = generator_state->capture_epilogue();
  }

  size_t numCUDAGraphNodes = 0;
  AT_CUDA_CHECK(cudaGraphGetNodes(graph_, nullptr, &numCUDAGraphNodes));
  if (numCUDAGraphNodes == 0) {
      TORCH_WARN("The CUDA Graph is empty. This usually means that the graph was ",
                 "attempted to be captured on wrong device or stream.");
  }

  {
    std::unique_lock<std::mutex> lock(_currently_capturing_graphs_mutex);
    _currently_capturing_graphs.erase(capture_id_);
  }

  capture_ended_ = true;
  has_graph_ = true;
  if (!keep_graph_) {
    instantiate();
    if (!_cuda_graphs_debug) {
      AT_CUDA_CHECK(cudaGraphDestroy(graph_));
    }
    has_graph_ = false;
  }
}

void CUDAGraph::instantiate() {
  TORCH_CHECK(capture_ended_, "capture_end() must have been called before calling instantiate");

  if (has_graph_exec_) {
    TORCH_CHECK(keep_graph_, "instantiate() is intended to be called by the user only when keep_graph=true");
    AT_CUDA_CHECK(cudaGraphExecDestroy(graph_exec_));
  }

  if (dynamic_graph_) {
    // skip all instantiaton and such
    return;
  }

  // In typical graph usage some tensors (e.g. the tensors used for graph IO) are not freed
  // between replays.
  // If Pytorch compiles and runs with a CUDA 11.4+ toolkit, there's a chance the allocator backend
  // is cudaMallocAsync.
  // cudaMallocAsync is generally graph-safe, but if some tensors are not freed between replays,
  // the graph's internal bookkeeping requires that we instantiate with
  // cudaGraphInstantiateFlagAutoFreeOnLaunch. See
  // cudaGraphLaunch
  // https://docs.nvidia.com/cuda/cuda-runtime-api/group__CUDART__GRAPH.html#group__CUDART__GRAPH_1g1accfe1da0c605a577c22d9751a09597
  // cudaGraphInstantiateWithFlags
  // https://docs.nvidia.com/cuda/cuda-runtime-api/group__CUDART__GRAPH.html#group__CUDART__GRAPH_1ga2c652a24ba93e52b99a47bec0888233
#if !defined(USE_ROCM) || ROCM_VERSION >= 60200
  int version = 0;
  AT_CUDA_CHECK(cudaDriverGetVersion(&version));
  if (version < 11040) {
#endif
    // Trailing NULL, NULL, 0 arguments were recommended by Cuda driver people,
    // who prefer not to report error message through these arguments moving forward
    // (they prefer return value, or errors on api calls internal to the capture)
#if (defined(CUDA_VERSION) && CUDA_VERSION >= 12000)
    AT_CUDA_CHECK(cudaGraphInstantiate(&graph_exec_, graph_, 0));
#else
    AT_CUDA_CHECK(cudaGraphInstantiate(&graph_exec_, graph_, NULL, NULL, 0));
#endif
//Since ROCm 6.2, we want to go down this path as hipGraphExecDestroy in the destructor will not immediately free the memory.
//It will wait for the next sync operation. cudaGraphInstantiateFlagAutoFreeOnLaunch will add async frees after graph launch.
#if !defined(USE_ROCM) || ROCM_VERSION >= 60200
  } else {
    AT_CUDA_CHECK(cudaGraphInstantiateWithFlags(&graph_exec_,
                                                graph_,
                                                cudaGraphInstantiateFlagAutoFreeOnLaunch));
  }
#endif
  has_graph_exec_ = true;
}

void CUDAGraph::replay() {
  TORCH_CHECK(capture_ended_,
              "Called CUDAGraph::replay without a preceding successful capture.");
  TORCH_CHECK(!dynamic_graph_, "Call replay_dynamic instead");

  if (!has_graph_exec_) {
    TORCH_INTERNAL_ASSERT(keep_graph_);
    instantiate();
  }

  c10::OptionalDeviceGuard device_guard{capture_stream_.device()};

  for (auto& [generator_state, wholegraph_increments] :
       captured_generator_states_) {
    generator_state->replay_prologue(wholegraph_increments);
  }
  // graph_exec_ may be replayed in any stream.
  AT_CUDA_CHECK(cudaGraphLaunch(graph_exec_, at::cuda::getCurrentCUDAStream()));

  int version = 0;
  AT_CUDA_CHECK(cudaDriverGetVersion(&version));
  if (version < 11040) {
    // Workaround for bug in libcuda.so that causes replayed graphs with
    // certain topologies to be corrupted (kernels elided, internal syncs
    // ignored) when replayed back to back without a sync in between.
    // The bug is fixed in CUDA 11.4+.
    AT_CUDA_CHECK(cudaDeviceSynchronize());
  }
}

void CUDAGraph::enable_debug_mode() {
  _cuda_graphs_debug = true;
}

void CUDAGraph::debug_dump(const std::string& debug_path) {
#if defined(CUDA_VERSION) || defined(USE_ROCM)
  if (_cuda_graphs_debug || keep_graph_) {
    TORCH_WARN("DEBUG: calling debug_dump()");
    if (has_graph_) {
      TORCH_WARN("DEBUG: calling cudaGraphDebugDotPrint() with ", debug_path);
      C10_CUDA_CHECK_WARN(cudaGraphDebugDotPrint(graph_, debug_path.c_str(), cudaGraphDebugDotFlagsVerbose)); // most verbose output
      if (!keep_graph_) {
        AT_CUDA_CHECK(cudaGraphDestroy(graph_));
        has_graph_ = false;
      }
    }
  } else {
    TORCH_WARN("CUDA Graphs debug not enabled, set with [graph].enable_debug_mode()");
  }
#else
  TORCH_CHECK(false, "CUDA graphs may only be used in Pytorch built with CUDA >= 11.3 or ROCM >= 5.6");
#endif
}

cudaGraph_t CUDAGraph::raw_cuda_graph() {
  TORCH_CHECK(keep_graph_, "You cannot access the raw cudaGraph_t instance unless CUDAGraph was initialized with keep_graph=true");
  TORCH_CHECK(has_graph_, "You cannot access the raw cudaGraph_t instance until capture_end() has been called");
  return graph_;
}

void CUDAGraph::reset() {
  // I'd prefer these checks throw exceptions, not print warnings,
  // but the destructor calls reset(), and at least one CI build
  // refuses to compile with a throwing destructor.
  //
  // Instead of calling reset() in the destructor to clean up, I could
  // call reset() in the __del__ method of a thin Python wrapper,
  // in which case reset would be allowed to throw exceptions.
  // But Stackoverflow does not like user-defined __del__.
  // __del__ prevents Graph instances from EVER being garbage collected
  // if they participate in a reference cycle.
  // And exceptions thrown in __del__ only print a warning anyway.
  //
  // Calling reset() in the C++ destructor, with warnings instead of exceptions
  // if calls fail, is the compromise we chose.
  //
  // If capture_begin, the capture, or capture_end failed at some point, this CUDAGraph, the generator,
  // and the allocator could end up in all kinds of weird states depending where failure occurred.
  // If the user catches the failure exception in a script, or is running in REPL or (god forbid)
  // a Jupyter notebook, I don't see an easy way for reset() to gracefully fix all such possible error states.
  if (capture_ended_) {
    // notifyCaptureDestroy may throw. How should we handle this?
    c10::cuda::CUDACachingAllocator::releasePool(capture_dev_, mempool_id_);
    // I am cudaFreeing this pool's memory eagerly.
    c10::cuda::CUDACachingAllocator::emptyCache(mempool_id_);
    capture_ended_ = false;
  }
  if (has_graph_) {
    C10_CUDA_CHECK_WARN(cudaGraphDestroy(graph_));
    has_graph_ = false;
  }
  if (has_graph_exec_) {
    C10_CUDA_CHECK_WARN(cudaGraphExecDestroy(graph_exec_));
    has_graph_exec_ = false;
  }
  if (dynamic_graph_) {
    allocations_.clear();
    kernel_param_updates_.clear();
    graph_node_param_updates_.clear();
    host_memory_updates_.clear();
    dynamic_graph_ = false;
  }
}

// Returns an id another graph's capture_begin can use to share the same memory pool as this graph.
MempoolId_t CUDAGraph::pool() {
TORCH_CHECK(capture_ended_,
              "Called CUDAGraph::pool() without a preceding successful capture.");
  return mempool_id_;
}

CUDAGraph::~CUDAGraph() {
  for (auto& [generator_state, wholegraph_increments] :
       captured_generator_states_) {
    generator_state->unregister_graph(this);
  }
  reset();

// There are recent HIP changes where hipGraphExecDestroy doesn't immediately free memory.
// They wait for next sync point in order to free the memory, this is to ensure that all
// hipGraphLaunch are finished before we release any memory. This feature was enabled in rocm6.2.
// We need to ensure all async opreations finish before deleting the object.
#if (defined(USE_ROCM) && ROCM_VERSION >= 60200)
  if (capture_dev_ != UNDEFINED_DEVICE) // check if capture_dev_ contains the real device id
  {
    AT_CUDA_CHECK(cudaSetDevice(capture_dev_));
    AT_CUDA_CHECK(cudaDeviceSynchronize());
  }
#endif
}

void CUDAGraph::add_dynamic_update(const std::tuple<size_t, size_t, size_t>& result,
                                   cudaGraphNode_t node,
                                   size_t param_offset) {
  auto [alloc_idx, offset, address_start] = result;
  std::cout << "GALVEZ: Adding to node at param offset "  << param_offset + address_start << " for allocation " << (void*)allocations_[alloc_idx].ptr << " with size " << allocations_[alloc_idx].size << " and address offset of " << offset << std::endl;
  cudaKernelNodeAttrValue attr_value = {
    .deviceUpdatableKernelNode = {
      .deviceUpdatable = 1,
      .devNode = nullptr,
    }
  };
  AT_CUDA_CHECK(cudaGraphKernelNodeSetAttribute(
      node,
      cudaLaunchAttributeDeviceUpdatableKernelNode,
      &attr_value));
  TORCH_CHECK(attr_value.deviceUpdatableKernelNode.devNode != nullptr);

  kernel_param_updates_.push_back({
      .dev_node = attr_value.deviceUpdatableKernelNode.devNode,
      .param_offset = param_offset + address_start,
      .alloc_idx = alloc_idx,
      .offset = offset,
    });
}

void CUDAGraph::add_host_memory_update(size_t alloc_idx, void **address_to_update, size_t offset) {
  host_memory_updates_.push_back({alloc_idx, address_to_update, offset});
}


// pinned or pageable host memory can be written by the host at any
// time. This cannot be captured. Therefore, if any of the values
// within the memory copy buffer correspond to input or output
// pointers, we need to copy these as well.
// consider grouped gemm's implementation
// std::unordered_set<cudaGraphNode_t> collect_memcpy_host_to_device_nodes_that_mutate_inputs() {
  
// }

// this does not use the pointer diffing approach. Not a big fan of that...

// TODO: This will need to find the non dynamic tensors allocated by
// this graph, and then give them real backing allocations.
void CUDAGraph::become_dynamic(const std::vector<std::pair<void*, size_t>>& dynamic_tensors) {
  TORCH_CHECK(dynamic_graph_, "Graph must have been captured with dynamic_graph=True");
  TORCH_CHECK(allocations_.empty(), "Must not have already called become_dynamic");
  TORCH_CHECK(!dynamic_tensors.empty(), "Must have at least one dynamic tensor");
  TORCH_CHECK(has_graph_, "Must have already captured");
  TORCH_INTERNAL_ASSERT(!has_graph_exec_);

  std::cout << "become_dynamic() unbacked_memory size " << unbacked_memory.size() << std::endl;

  std::set<size_t, std::greater<size_t>> indices_to_remove;

  for (size_t i = 0; i < unbacked_memory.size(); ++i) {
    auto&& [ptr, size] = unbacked_memory[i];
    for(auto&& [dynamic_tensor_data_ptr, dynamic_tensor_nbytes]: dynamic_tensors) {
      if (regions_intersect(dynamic_tensor_data_ptr, dynamic_tensor_nbytes, ptr, size)) {
        indices_to_remove.insert(i);
        std::cout << "GALVEZ: removing index " << i <<  " with pointer " << ptr << " based on overlap with " << dynamic_tensor_data_ptr << std::endl;
      }
    }
  }

  // for (size_t i: indices_to_remove) {
  //   auto&& [ptr, size] = unbacked_memory[i];
  //   size_t size_rounded_up_to_page = roundUpToNearestTwoMiB(size);
  //   C10_CUDA_DRIVER_CHECK(c10::cuda::DriverAPI::get()->cuMemUnmap_((CUdeviceptr)ptr, size_rounded_up_to_page));
  //   C10_CUDA_DRIVER_CHECK(c10::cuda::DriverAPI::get()->cuMemAddressFree_((CUdeviceptr)ptr, size_rounded_up_to_page));
  // }

  for (size_t idx : indices_to_remove) {
    unbacked_memory.erase(unbacked_memory.begin() + idx);
  }

  std::cout << "become_dynamic() unbacked_memory size " << unbacked_memory.size() << std::endl;

  for (size_t i = 0; i < dynamic_tensors.size(); i++) {
    auto const& [tensor_data_ptr, tensor_nbytes] = dynamic_tensors[i];
    // TODO: Reconsider this requirement
    // TORCH_CHECK(tensor.is_contiguous(), "Dynamic tensors must be contiguous");
    allocations_.push_back(DynamicGraphAllocation{
      .ptr = (char*) tensor_data_ptr,
      .size = tensor_nbytes,
      .alloc_idx = i, // record the original order, since the user will use that order again in replay_dynamic
    });
  }

  std::vector<DynamicGraphAllocation> sorted_allocations = allocations_;
  // copy since we're going to mess up allocation order, just for this function
  // we will need to look up a bunch of pointers, and binary search can speed this up from linear to log time
  // therefore:
  std::sort(sorted_allocations.begin(), sorted_allocations.end(), [](auto& a, auto& b) {
    return a.ptr < b.ptr;
  });

  for (size_t i = 1; i < sorted_allocations.size(); i++) {
    auto prev = sorted_allocations[i - 1];
    TORCH_CHECK(prev.ptr + prev.size <= sorted_allocations[i].ptr, "Dynamic tensors may not overlap");
  }

  // Iterate over all pointers made by this CUDAGraph. It is my
  // responsibility to allocate them by unmapping the dummy TWO_MIB
  // page, and then remapping physical memory of the same size as the
  // virtual memory.

  // TODO: Think about how this might interact with NCCL's
  // NCCL_GRAPH_REGISTER config. Does registration get messed up when
  // we delete the backing physical memory? I hope not.

  size_t num_nodes;
  AT_CUDA_CHECK(cudaGraphGetNodes(graph_, nullptr, &num_nodes));
  std::vector<cudaGraphNode_t> nodes(num_nodes);
  if (num_nodes != 0) {
    // return cudaErrorInvalidValue if num_nodes is 0
    AT_CUDA_CHECK(cudaGraphGetNodes(graph_, nodes.data(), &num_nodes));
  }

  for (size_t i = 0; i < num_nodes; ++i) {
    cudaGraphNode_t node = nodes[i];
    std::cout << "GALVEZ: investigating node " << i << std::endl;

    cudaGraphNodeType type;
    AT_CUDA_CHECK(cudaGraphNodeGetType(node, &type));

    if (type == cudaGraphNodeTypeKernel) {
      CUDA_KERNEL_NODE_PARAMS driver_node_params;
      AT_CUDA_DRIVER_CHECK(
          globalContext().getNVRTC().cuGraphKernelNodeGetParams(
              node, &driver_node_params));
      // Runtime API has a problem with the triton kernels loaded by
      // cuModuleLoadData.  It tries to convert the CUfunc to a void*
      // by looking up the entrypoint that the driver API normally
      // generates for a CUDA kernel. However, there is no such entry
      // point for kernel loaded via the cuModuleLoad* APIs IIUC. We
      // just convert back to a CUfunc/cudaFunction_t anyway, so using
      // the driver API is fine here.

      // cudaKernelNodeParams nodeParams;
      // AT_CUDA_CHECK(cudaGraphKernelNodeGetParams(node, &nodeParams));
      // AT_CUDA_CHECK(cudaGetFuncBySymbol(&func, nodeParams.func));

      cudaFunction_t func = driver_node_params.func;

      const char* func_name;
      AT_CUDA_DRIVER_CHECK(
          globalContext().getNVRTC().cuFuncGetName(&func_name, func));

      

      TORCH_CHECK(
          driver_node_params.kernelParams && !driver_node_params.extra,
          "Kernel launches that use `extra` instead of `kernelParams` are not supported");

      size_t param_offset;
      size_t param_size;

      ArgumentInformation type_info = get_argument_information(func);
      for (size_t param_index = 0;
           globalContext().getNVRTC().cuFuncGetParamInfo(func, param_index, &param_offset, &param_size) != CUDA_ERROR_INVALID_VALUE; param_index++) {
        if (type_info.members.empty()) {
          if (param_index == 0) {
            // TORCH_WARN("No type information for", func_name);
            std::cout << "No type information for" << func_name << std::endl;
          }
          char** arg1_speculative_pointer =
            (char**)driver_node_params.kernelParams[param_index];

          // ABI guarantees that pointers have 8-byte alignment
          for (size_t address_start = 0; param_size - address_start >= 8;
               address_start += 8) {
            char* arg1_value = arg1_speculative_pointer[address_start / 8];
            if (auto result = checkAllocationWithinGraph(arg1_value, sorted_allocations)) {
              add_dynamic_update({std::get<0>(*result), std::get<1>(*result), address_start},
                                 node, param_offset);
            }
          }
        } else {
          // check using type information
          char* arg =
            (char*)driver_node_params.kernelParams[param_index];

          // is this empty for some reason?
          std::vector<std::tuple<size_t, size_t, size_t>> results =
            gather_pointers_in_node(type_info.members[param_index].second, arg, sorted_allocations, 0);

          for (auto&& result: results) {
            add_dynamic_update(result, node, param_offset);
          }
        }
      }
    } else if (type == cudaGraphNodeTypeMemcpy) {
      TORCH_INTERNAL_ASSERT(false, "memcpy nodes should have been removed");
      cudaMemcpy3DParms memcpyParams1;
      AT_CUDA_CHECK(cudaGraphMemcpyNodeGetParams(node, &memcpyParams1));

      auto srcPtrResult = checkAllocationWithinGraph(memcpyParams1.srcPtr.ptr, sorted_allocations);
      auto dstPtrResult = checkAllocationWithinGraph(memcpyParams1.dstPtr.ptr, sorted_allocations);

      // both are device pointers in my inputs or outputs
      if (srcPtrResult || dstPtrResult) {
      graph_node_param_updates_.push_back({
        .node = node,
        .compute_new_params = [memcpyParams1, srcPtrResult, dstPtrResult](std::vector<void*> actualDataPtrs) {
          std::cout << "GALVEZ: memcpy setting" << std::endl;
          cudaPitchedPtr srcPtr = memcpyParams1.srcPtr;
          cudaPitchedPtr dstPtr = memcpyParams1.dstPtr;

          cudaPointerAttributes attributes;
          AT_CUDA_CHECK(cudaPointerGetAttributes(&attributes, srcPtr.ptr));
          std::cout << "GALVEZ: cudaPointerGetAttributes original device pointer " << attributes.devicePointer << " host pointer " << attributes.hostPointer << " memory type " << attributes.type << " device " << attributes.device << std::endl;
          
          if (srcPtrResult) {
            auto [alloc_idx, offset] = *srcPtrResult;
            srcPtr.ptr = (char*)actualDataPtrs[alloc_idx] + offset;

            cudaPointerAttributes attributes;
            AT_CUDA_CHECK(cudaPointerGetAttributes(&attributes, dstPtr.ptr));
            std::cout << "GALVEZ: cudaPointerGetAttributes replaced device pointer " << attributes.devicePointer << " host pointer " << attributes.hostPointer << " memory type " << attributes.type << " device " << attributes.device << std::endl;
          }
          if (dstPtrResult) {
            auto [alloc_idx, offset] = *dstPtrResult;
            dstPtr.ptr = (char*)actualDataPtrs[alloc_idx] + offset;
          }
          return cudaGraphNodeParams{
            .type = cudaGraphNodeTypeMemcpy,
            .memcpy = {
              .copyParams = {
                .srcArray = memcpyParams1.srcArray,
                .srcPos = memcpyParams1.srcPos,
                .srcPtr = srcPtr,
                .dstArray = memcpyParams1.dstArray,
                .dstPos = memcpyParams1.dstPos,
                .dstPtr = dstPtr,
                .extent = memcpyParams1.extent,
                .kind = memcpyParams1.kind
              },
            },
          };
        },
      });
      } else {
        cudaPointerAttributes src_attr, dst_attr;
        AT_CUDA_CHECK(cudaPointerGetAttributes(&src_attr, memcpyParams1.srcPtr.ptr));
        AT_CUDA_CHECK(cudaPointerGetAttributes(&dst_attr, memcpyParams1.dstPtr.ptr));

        // we need to modify the host memory itself here...
        if (src_attr.type == cudaMemoryTypeHost) {
          void *aligned_address = align_to_8_bytes(memcpyParams1.srcPtr.ptr);
          void **speculative_pointer = (void**)aligned_address;
          size_t buffer_length = memcpyParams1.extent.width *
            memcpyParams1.extent.height * memcpyParams1.extent.depth;
          for (size_t address_start = 0; buffer_length - address_start >= 8;
               address_start += 8) {
            void* arg1_value = speculative_pointer[address_start / 8];
            if (auto result = checkAllocationWithinGraph(arg1_value, sorted_allocations)) {
              auto [alloc_idx, offset] = *result;
              add_host_memory_update(alloc_idx, &speculative_pointer[address_start / 8], offset);
            }
          }
        }
      }
    } else if (type == cudaGraphNodeTypeMemset) {
      TORCH_INTERNAL_ASSERT(false, "memcpy nodes should have been removed");
      cudaMemsetParams memsetParams1;
      AT_CUDA_CHECK(cudaGraphMemsetNodeGetParams(node, &memsetParams1));

      auto dstPtrResult = checkAllocationWithinGraph(memsetParams1.dst, sorted_allocations);
      if (!dstPtrResult) {
        continue;
      }

      graph_node_param_updates_.push_back({
        .node = node,
        .compute_new_params = [memsetParams1, dstPtrResult](std::vector<void*> actualDataPtrs) {
          std::cout << "GALVEZ: memset setting" << std::endl;
          void* dstPtr = memsetParams1.dst;
          if (dstPtrResult) {
            auto [alloc_idx, offset] = *dstPtrResult;
            dstPtr = (char*)actualDataPtrs[alloc_idx] + offset;
          }
          return cudaGraphNodeParams{
            .type = cudaGraphNodeTypeMemset,
            .memset = {
              .dst = dstPtr,
              .pitch = memsetParams1.pitch,
              .value = memsetParams1.value,
              .elementSize = memsetParams1.elementSize,
              .width = memsetParams1.width,
              .height = memsetParams1.height,
            },
          };
        },
      });
    } else if (type == cudaGraphNodeTypeGraph || type == cudaGraphNodeTypeConditional) {
      throw std::runtime_error("horrible. please don't make me support these.");
      assert(false);
    } else if (
      type == cudaGraphNodeTypeEmpty ||
      type == cudaGraphNodeTypeWaitEvent ||
      type == cudaGraphNodeTypeHost ||
      type == cudaGraphNodeTypeEventRecord ||
      type == cudaGraphNodeTypeExtSemaphoreSignal ||
      type == cudaGraphNodeTypeExtSemaphoreWait ||
      type == cudaGraphNodeTypeMemAlloc ||
      type == cudaGraphNodeTypeMemFree
    ) {
      // this is fine
    } else {
      throw std::runtime_error("graph node type unknown");
      assert(false);
    }
  }
  AT_CUDA_CHECK(cudaGraphInstantiate(&graph_exec_, graph_, 0));

  // We must call cudaGraphUpload() to allocate memory for the
  // updatable device nodes. Otherwise, cudaGraphLaunch will
  // fail.

  // From
  // https://docs.nvidia.com/cuda/cuda-runtime-api/group__CUDART__GRAPH.html#group__CUDART__GRAPH_1ge546432e411b4495b93bdcbf2fc0b2bd:
  // "[cudaGraphUpload] Uses memory cached by stream to back the allocations
  // owned by graphExec."

  // Normally, cudaGraphUpload() must be called on the stream for that
  // will do cudaGraphLaunch(). However, here we can actually use any
  // arbitrary stream (thus, the stream we caputred on works) for the
  // memory backing device nodes. This differs from the usual case for
  // graphs using memory allocation and free nodes. TODO: Figure out
  // precisely why.

  // Can this stream ever be deleted while still retain a reference to it?
  // Yes, if an external stream is used. That is probably very rare.
  // I suppose we know that torch streams will never be deallocated, so this is
  // fine...
  AT_CUDA_CHECK(cudaGraphUpload(graph_exec_, capture_stream_));
  capture_stream_.synchronize();

  std::cout << "become_dynamic() unbacked_memory size " << unbacked_memory.size() << std::endl;

  for (auto&& [ptr, size]: unbacked_memory) {
    size_t size_rounded_up_to_page = roundUpToNearestTwoMiB(size);
    // wait, so will this unmap every single handle that was mapped to
    // each 2 MiB page? I'm not sure...
    C10_CUDA_DRIVER_CHECK(c10::cuda::DriverAPI::get()->cuMemUnmap_((CUdeviceptr)ptr, size_rounded_up_to_page));

    CUmemGenericAllocationHandle handle{};
    CUmemAllocationProp prop{};
    prop.type = CU_MEM_ALLOCATION_TYPE_PINNED;
    prop.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    prop.location.id = capture_dev_;
    C10_CUDA_DRIVER_CHECK(c10::cuda::DriverAPI::get()->cuMemCreate_(&handle, size_rounded_up_to_page, &prop, 0ULL));
    C10_CUDA_DRIVER_CHECK(c10::cuda::DriverAPI::get()->cuMemMap_((CUdeviceptr)ptr, size_rounded_up_to_page, 0, handle, 0ULL));
    CUmemAccessDesc desc{};
    desc.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    // NOLINTNEXTLINE(bugprone-signed-char-misuse)
    desc.location.id = capture_dev_;
    // we are marking this page inaccessible because we should never
    // actually access it. It should always be replaced by parameterized
    // graph launch
    desc.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
    C10_CUDA_DRIVER_CHECK(c10::cuda::DriverAPI::get()->cuMemSetAccess_((CUdeviceptr)ptr, size_rounded_up_to_page /* can I use size? */, &desc, 1));
  }
  // unbacked_memory.clear();
  
  has_graph_exec_ = true;
}

template<bool VOLTA_OR_LATER>
void CUDAGraph::launch_dynamic_updaters(const std::vector<void*>& actualDataPtrs) {
  using KernelUpdateSOA = KernelUpdateSOA<VOLTA_OR_LATER>;
  static_assert(KernelUpdateSOA::MAX_NUM_UPDATES > 0);
  static_assert(sizeof(KernelUpdateSOA) <= KernelUpdateSOA::KERNEL_PARAM_LIMIT_BYTES);
  static_assert(sizeof(KernelUpdateSOA) + KernelUpdateSOA::PER_UPDATE > KernelUpdateSOA::KERNEL_PARAM_LIMIT_BYTES);

  for (size_t off = 0; off < kernel_param_updates_.size(); off += KernelUpdateSOA::MAX_NUM_UPDATES) {
    // (overwhelmingly likely that this loop only runs once)
    size_t num_updates = std::min(KernelUpdateSOA::MAX_NUM_UPDATES, kernel_param_updates_.size() - off);
    KernelUpdateSOA update_soa{
      .num_updates = num_updates,
    };
    for (size_t i = 0; i < num_updates; i++) {
      auto update = kernel_param_updates_[off + i];
      update_soa.device_nodes[i] = update.dev_node;
      update_soa.new_pointers[i] =
          (char*)actualDataPtrs[update.alloc_idx] + update.offset;
      update_soa.param_offsets[i] = update.param_offset;
    }
    dynamic_graph_updater<VOLTA_OR_LATER>(update_soa);
  }
}

void CUDAGraph::replay_dynamic(const std::vector<at::Tensor>& dynamic_tensors) {
  TORCH_CHECK(dynamic_graph_, "Must be a dynamic graph");
  TORCH_CHECK(has_graph_exec_, "Called CUDAGraph::replay_dynamic without a preceding successful capture.");
  TORCH_CHECK(!allocations_.empty(), "Must have already called become_dynamic");
  TORCH_CHECK(dynamic_tensors.size() == allocations_.size(), "Must pass the same number of tensors as are dynamic");

  if (!has_graph_exec_) {
    TORCH_INTERNAL_ASSERT(keep_graph_);
    instantiate();
  }

  c10::OptionalDeviceGuard device_guard{capture_stream_.device()};
  cudaStream_t stream = at::cuda::getCurrentCUDAStream();

  // take the allocations that were requested during the capture, and allocate them "for real"
  std::vector<void*> actualDataPtrs;
  for (size_t i = 0; i < allocations_.size(); i++) {
    TORCH_INTERNAL_ASSERT(allocations_[i].alloc_idx == i);
    // TORCH_CHECK(dynamic_tensors[i].is_contiguous(), "All tensors must be contiguous");
    // TODO ^ this isn't quite right. really, the assert should be that the stride is the same as the original input arg.
    // TORCH_CHECK(dynamic_tensors[i].nbytes() == allocations_[i].size, "Prefilled tensors must be same shape");
    actualDataPtrs.push_back(dynamic_tensors[i].data_ptr());
  }

  // TODO: Do we need to synchronize before doing this?
  for (auto& update : graph_node_param_updates_) {
    // run the updates that can't be done device-side
    // note that this is quite rare - it only applies to a few misc memsets/memcpys
    
    cudaGraphNodeParams newParams = update.compute_new_params(actualDataPtrs);
    AT_CUDA_CHECK(cudaGraphExecNodeSetParams(graph_exec_, update.node, &newParams));
  }

  // we have to do these updates on host, not device, since some
  // memory might be pageable rather than pinned.
  for (auto&& [alloc_idx, address_to_update, offset] : host_memory_updates_) {
    *address_to_update = (void*)((char*)actualDataPtrs[alloc_idx] + offset);
  }

  // do we need to cache this call somehow?
  if (at::cuda::getCurrentDeviceProperties()->major >= 7) {
    launch_dynamic_updaters<true>(actualDataPtrs);
  } else {
    launch_dynamic_updaters<false>(actualDataPtrs);
  }
 
 AT_CUDA_CHECK(cudaGraphLaunch(graph_exec_, stream));
}

bool dim3_equal(const dim3& a, const dim3& b) {
  return a.x == b.x && a.y == b.y && a.z == b.z;
}

void check_differences(void *buf1, void *buf2, size_t n) {
    // Cast the void pointers to unsigned char pointers for byte-wise access
    unsigned char* a = static_cast<unsigned char*>(buf1);
    unsigned char* b = static_cast<unsigned char*>(buf2);

    size_t i = 0;
    while (i < n) {
        // If the current bytes differ, mark the start of a difference region.
        if (a[i] != b[i]) {
            size_t start = i;
            // Continue advancing until we hit bytes that are the same or the end.
            while (i < n && a[i] != b[i])
                ++i;
            // Print the region where differences were found in the [start, end) format.
            std::cout << "GALVEZ wrong region: [" << start << ", " << i << ")\n";
        } else {
            ++i;
        }
    }
}

/**
 * Check equality between two cudaMemsetParams structs
 * @param a First cudaMemsetParams struct
 * @param b Second cudaMemsetParams struct
 * @return true if all members are equal, false otherwise
 */
bool is_equal(const cudaMemsetParams& a, const cudaMemsetParams& b) {
    return a.dst == b.dst &&
           a.pitch == b.pitch &&
           a.value == b.value &&
           a.elementSize == b.elementSize &&
           a.width == b.width &&
           a.height == b.height;
}

/**
 * Check equality between two cudaMemcpy3DParms structs
 * @param a First cudaMemcpy3DParms struct
 * @param b Second cudaMemcpy3DParms struct
 * @return true if all members are equal, false otherwise
 */
bool is_equal(const cudaMemcpy3DParms& a, const cudaMemcpy3DParms& b) {
    // Compare array pointers
    if (a.srcArray != b.srcArray || a.dstArray != b.dstArray)
        return false;
    
    // Compare positions
    if (a.srcPos.x != b.srcPos.x || a.srcPos.y != b.srcPos.y || a.srcPos.z != b.srcPos.z ||
        a.dstPos.x != b.dstPos.x || a.dstPos.y != b.dstPos.y || a.dstPos.z != b.dstPos.z)
        return false;
    
    // Compare pitched pointers
    if (a.srcPtr.ptr != b.srcPtr.ptr || a.srcPtr.pitch != b.srcPtr.pitch ||
        a.srcPtr.xsize != b.srcPtr.xsize || a.srcPtr.ysize != b.srcPtr.ysize ||
        a.dstPtr.ptr != b.dstPtr.ptr || a.dstPtr.pitch != b.dstPtr.pitch ||
        a.dstPtr.xsize != b.dstPtr.xsize || a.dstPtr.ysize != b.dstPtr.ysize)
        return false;
    
    // Compare extent
    if (a.extent.width != b.extent.width || a.extent.height != b.extent.height ||
        a.extent.depth != b.extent.depth)
        return false;
    
    // Compare kind
    return a.kind == b.kind;
}

#include <iostream>
#include <iomanip>   // for std::hex, std::setw, std::setfill
#include <cstddef>   // for std::size_t

// Prints the first n bytes starting at 'data' as a hex string (e.g. "0a1f2b...")
void printHex(const void* data, std::size_t n) {
    // Treat the data as a sequence of unsigned bytes
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    
    // Set up cout for hex, two digits per byte, zero-padded
    std::cout << std::hex << std::setfill('0');
    
    for (std::size_t i = 0; i < n; ++i) {
        // Each byte is cast to an unsigned int so it's printed as a number,
        // then setw(2) ensures two hex digits (e.g. 0a, ff, etc.)
        std::cout << std::setw(2) << static_cast<unsigned int>(bytes[i]);
    }
    
    // Go back to decimal for any further output
    std::cout << std::dec;
}


bool graphs_equal(cudaGraph_t graph1, cudaGraph_t graph2) {
  TORCH_CHECK(graph1 != graph2);

  bool is_equal_var = true;
  
  size_t num_nodes1;
  AT_CUDA_CHECK(cudaGraphGetNodes(graph1, nullptr, &num_nodes1));

  size_t num_nodes2;
  AT_CUDA_CHECK(cudaGraphGetNodes(graph2, nullptr, &num_nodes2));

  if(num_nodes1 != num_nodes2) {
    TORCH_WARN("graphs_equal: number of nodes mismatch: graph1 has ", num_nodes1,
               " nodes, graph2 has ", num_nodes2, " nodes.");
    return false;
  }

  std::vector<cudaGraphNode_t> nodes1(num_nodes1);
  AT_CUDA_CHECK(cudaGraphGetNodes(graph1, nodes1.data(), &num_nodes1));

  std::vector<cudaGraphNode_t> nodes2(num_nodes2);
  AT_CUDA_CHECK(cudaGraphGetNodes(graph2, nodes2.data(), &num_nodes2));

  std::size_t num_edges1, num_edges2;
  AT_CUDA_CHECK(cudaGraphGetEdges(graph1, nullptr, nullptr, &num_edges1));
  AT_CUDA_CHECK(cudaGraphGetEdges(graph2, nullptr, nullptr, &num_edges2));

  if (num_edges1 != num_edges2) {
    TORCH_WARN("graphs_equal: number of edges mismatch: graph1 has ", num_edges1,
               " edges, graph2 has ", num_edges2, " edges.");
    return false;
  }

  for (size_t i = 0; i < num_nodes1; ++i) {
    cudaGraphNode_t node1 = nodes1[i];
    cudaGraphNode_t node2 = nodes2[i];

    cudaGraphNodeType type1, type2;
    AT_CUDA_CHECK(cudaGraphNodeGetType(node1, &type1));
    AT_CUDA_CHECK(cudaGraphNodeGetType(node2, &type2));
    if (type1 != type2) {
      TORCH_WARN("graphs_equal: Node type mismatch at index ", i,
                 ": graph1 type = ", type1, ", graph2 type = ", type2);
      return false;
    }

    if (type1 == cudaGraphNodeTypeKernel) {
      cudaKernelNodeParams nodeParams1, nodeParams2;
      AT_CUDA_CHECK(cudaGraphKernelNodeGetParams(node1, &nodeParams1));
      AT_CUDA_CHECK(cudaGraphKernelNodeGetParams(node2, &nodeParams2));
      if (nodeParams1.func != nodeParams2.func) {
        TORCH_WARN("graphs_equal: Kernel function mismatch at node index ", i,
                   ": graph1 function pointer = ", nodeParams1.func,
                   ", graph2 function pointer = ", nodeParams2.func);
        return false;
      }
      cudaFunction_t func1;
      AT_CUDA_CHECK(cudaGetFuncBySymbol(&func1, nodeParams1.func));

      const char* func_name;
      globalContext().getNVRTC().cuFuncGetName(&func_name, func1);

      std::cout << "GALVEZ: kernel name=" << func_name << std::endl;

      if (!dim3_equal(nodeParams1.gridDim, nodeParams2.gridDim)) {
        TORCH_WARN("graphs_equal: Kernel gridDim mismatch at node index ", i);
        return false;
      }
      if (!dim3_equal(nodeParams1.blockDim, nodeParams2.blockDim)) {
        TORCH_WARN("graphs_equal: Kernel blockDim mismatch at node index ", i);
        return false;
      }
      if (nodeParams1.sharedMemBytes != nodeParams2.sharedMemBytes) {
        TORCH_WARN("graphs_equal: Kernel shared memory bytes mismatch at node index ", i,
                   ": graph1 sharedMemBytes = ", nodeParams1.sharedMemBytes,
                   ", graph2 sharedMemBytes = ", nodeParams2.sharedMemBytes);
        return false;
      }

      size_t param_offset;
      size_t param_size;

      ArgumentInformation type_info = get_argument_information(func1);
      for (size_t param_index = 0;
           globalContext().getNVRTC().cuFuncGetParamInfo(func1, param_index, &param_offset, &param_size) != CUDA_ERROR_INVALID_VALUE; param_index++) {

        if (type_info.members.empty()) {
          if (param_index == 0) {
            TORCH_WARN("No type information for", func_name);
          }
          if (std::memcmp(nodeParams1.kernelParams[param_index],
                          nodeParams2.kernelParams[param_index],
                          param_size) != 0) {
            TORCH_WARN("graphs_equal: Kernel parameter mismatch at node index ", i,
                       ", parameter index ", param_index, " for function ", func_name);
            check_differences(nodeParams1.kernelParams[param_index], nodeParams2.kernelParams[param_index], param_size);
            is_equal_var = false;
            // return false;
          }
        } else {
          // TORCH_INTERNAL_ASSERT(type_info.at(func_name).size() == 1);
          if (!is_equal(nodeParams1.kernelParams[param_index],
                        nodeParams2.kernelParams[param_index],
                        type_info.members[param_index].second)) {

            // TODO: Double check whether we have a void * "data" field.
            TORCH_WARN("graphs_equal: Have type information, but Kernel parameter mismatch at node index ", i,
                       ", parameter index ", param_index, " for function ", func_name);
            // check_differences(nodeParams1.kernelParams[param_index], nodeParams2.kernelParams[param_index], param_size);
            // std::vector<ArgumentInformation> type_info_dup = get_argument_information(func1);
            TORCH_WARN("type info");
            prettyPrintArgumentInfo(type_info);
            TORCH_WARN("end type info");
            is_equal_var = false;


            std::cout << "First argument" << std::endl;
            printHex(nodeParams1.kernelParams[param_index], param_size);
            std::cout << std::endl;
            std::cout << "Second argument" << std::endl;
            printHex(nodeParams2.kernelParams[param_index], param_size);
            std::cout << std::endl;
            // return false;
          }
        }
      }
    } else if (type1 == cudaGraphNodeTypeMemcpy) {
      cudaMemcpy3DParms nodeParams1, nodeParams2;
      AT_CUDA_CHECK(cudaGraphMemcpyNodeGetParams(node1, &nodeParams1));
      AT_CUDA_CHECK(cudaGraphMemcpyNodeGetParams(node2, &nodeParams2));
      if (!is_equal(nodeParams1, nodeParams2)) {
        return false;
      }
    } else if (type1 == cudaGraphNodeTypeMemset) {
      cudaMemsetParams nodeParams1, nodeParams2;
      AT_CUDA_CHECK(cudaGraphMemsetNodeGetParams(node1, &nodeParams1));
      AT_CUDA_CHECK(cudaGraphMemsetNodeGetParams(node2, &nodeParams2));
      if (!is_equal(nodeParams1, nodeParams2)) {
        return false;
      }
    } else if (type1 == cudaGraphNodeTypeHost) {
      // This one is tricky. We don't know how to interpret the void*
      // userArgs field. We can check that the function is the
      // same... But we won't be able to, say, mutate any fields
      // inside of this. How can we handle this one? It is sensible to
      // see how nccl creates its host nodes, since that is the one
      // case that pytorch cares about right now...
      
      // We know for a fact that nccl always passes the type
      // ncclKernelPlan as its single argument
    } else if (type1 == cudaGraphNodeTypeGraph) {
      // Do recursion, but this case does not exist in pytorch at this time.
      TORCH_INTERNAL_ASSERT(false, "Unexpected node type");
      // return graphs_equal();
    } else if (type1 == cudaGraphNodeTypeWaitEvent) {
      // External events are not used yet in pytorch. The challenge
      // with it is that we unfortunately cannot assume that both
      // events are the same. This is a bit funky, since presumably
      // external events are intended to be passed to other functions,
      // which will either record or wait on this event. I definitely
      // need to consider this situation carefully.
      TORCH_INTERNAL_ASSERT(false, "Unexpected node type");
    } else {
      TORCH_INTERNAL_ASSERT(false, "Unexpected node type");
    }
  }
  // return true;
  return is_equal_var;
}

// bool  CUDAGraph::get_differences(cudaGraph_t graph1, cudaGraph_t graph2,
//                              const std::vector<at::Tensor>& dynamic_tensors1,
//                              const std::vector<at::Tensor>& dynamic_tensors2) {
//   TORCH_CHECK(dynamic_tensors1.size() == dynamic_tensors2.size());
//   TORCH_CHECK(graph1 != graph2);

//   // fill in kernel_param_updates_ and graph_node_param_updates_
  
//   size_t num_nodes1;
//   AT_CUDA_CHECK(cudaGraphGetNodes(graph1, nullptr, &num_nodes1));

//   size_t num_nodes2;
//   AT_CUDA_CHECK(cudaGraphGetNodes(graph2, nullptr, &num_nodes2));

//   if(num_nodes1 != num_nodes2) {
//     return false;
//   }

//   std::vector<cudaGraphNode_t> nodes1(num_nodes1);
//   AT_CUDA_CHECK(cudaGraphGetNodes(graph1, nodes1.data(), &num_nodes1));

//   std::vector<cudaGraphNode_t> nodes2(num_nodes2);
//   AT_CUDA_CHECK(cudaGraphGetNodes(graph2, nodes2.data(), &num_nodes2));

//   std::size_t num_edges1, num_edges2;
//   AT_CUDA_CHECK(cudaGraphGetEdges(graph1, nullptr, nullptr, &num_edges1));
//   AT_CUDA_CHECK(cudaGraphGetEdges(graph2, nullptr, nullptr, &num_edges2));

//   if (num_edges1 != num_edges2) {
//     return false;
//   }

//   for (size_t i = 0; i < num_nodes1; ++i) {
//     cudaGraphNode_t node1 = nodes1[i];
//     cudaGraphNode_t node2 = nodes2[i];

//     cudaGraphNodeType type1, type2;
//     AT_CUDA_CHECK(cudaGraphNodeGetType(node1, &type1));
//     AT_CUDA_CHECK(cudaGraphNodeGetType(node2, &type2));
//     if (type1 != type2) {
//       return false;
//     }

//     if (type1 == cudaGraphNodeTypeKernel) {
//       cudaKernelNodeParams nodeParams1, nodeParams2;
//       AT_CUDA_CHECK(cudaGraphKernelNodeGetParams(node1, &nodeParams1));
//       AT_CUDA_CHECK(cudaGraphKernelNodeGetParams(node2, &nodeParams2));
//       if (nodeParams1.func != nodeParams2.func) {
//         return false;
//       }
//       cudaFunction_t func1;
//       AT_CUDA_CHECK(cudaGetFuncBySymbol(&func1, nodeParams1.func));

//       const char* func_name;
//       globalContext().getNVRTC().cuFuncGetName(&func_name, func1);

//       std::cout << "GALVEZ: kernel name=" << func_name << std::endl;

//       if (!dim3_equal(nodeParams1.gridDim, nodeParams2.gridDim)) {
//         return false;
//       }
//       if (!dim3_equal(nodeParams1.blockDim, nodeParams2.blockDim)) {
//         return false;
//       }
//       if (nodeParams1.sharedMemBytes != nodeParams2.sharedMemBytes) {
//         return false;
//       }

//       std::cout << "GALVEZ:" << ((float*)nodeParams1.kernelParams[2]) << " " << ((float*)nodeParams1.kernelParams[2] + 1) << std::endl;
//       std::cout << "GALVEZ:" << ((float*)nodeParams2.kernelParams[2]) << " " << ((float*)nodeParams2.kernelParams[2] + 1) << std::endl;

//       size_t param_offset;
//       size_t param_size;
//       for (size_t param_index = 0;
//            globalContext().getNVRTC().cuFuncGetParamInfo(func1, param_index, &param_offset, &param_size) != CUDA_ERROR_INVALID_VALUE; param_index++) {
//         std::cout << "GALVEZ:" << param_index << " " << param_offset << " " << param_size << std::endl;

//         if (std::memcmp(nodeParams1.kernelParams[param_index], nodeParams2.kernelParams[param_index], param_size) != 0) {
//           return false;
//         }
//       }
//     } else if (type1 == cudaGraphNodeTypeMemcpy) {
//       cudaMemcpyNodeParams nodeParams1, nodeParams2;
//       // todo make an llm write the rest of these equality
//       // comparisons. Does C++ generate == implementations? I don't
//       // think so. Correct, it doesn't.
//     } else if (type1 == cudaGraphNodeTypeMemset) {
      
//     } else if (type1 == cudaGraphNodeTypeHost) {
//       // This one is tricky. We don't know how to interpret the void*
//       // userArgs field. We can check that the function is the
//       // same... But we won't be able to, say, mutate any fields
//       // inside of this. How can we handle this one? It is sensible to
//       // see how nccl creates its host nodes, since that is the one
//       // case that pytorch cares about right now...
      
//       // We know for a fact that nccl always passes the type
//       // ncclKernelPlan as its single argument
//     } else if (type1 == cudaGraphNodeTypeGraph) {
//       // Do recursion, but this case does not exist in pytorch at this time.
//       TORCH_INTERNAL_ASSERT(false, "Unexpected node type");
//       // return graphs_equal();
//     } else if (type1 == cudaGraphNodeTypeWaitEvent) {
//       // External events are not used yet in pytorch. The challenge
//       // with it is that we unfortunately cannot assume that both
//       // events are the same. This is a bit funky, since presumably
//       // external events are intended to be passed to other functions,
//       // which will either record or wait on this event. I definitely
//       // need to consider this situation carefully.
//       TORCH_INTERNAL_ASSERT(false, "Unexpected node type");
//     } else {
//       TORCH_INTERNAL_ASSERT(false, "Unexpected node type");
//     }
//   }
//   return true;
// }


bool operator==(const CUDAGraph& left, const CUDAGraph& right) {
    return graphs_equal(left.graph_, right.graph_);
  }

bool operator!=(const CUDAGraph& left, const CUDAGraph& right) {
    return !(left == right);
  }

  // my invariants:
  
  // 1. One memory pool per graph. No more sharing memory pools like
  // cuda graph trees. (Can we relax this?)
  
  // 2. There is a point at which you are done allocating, at which
  // point you are just replacing physical memory or deallocating.

  // need map of base pointer to a vector of std::shared_ptr<physical
  // memory handle>

  // I should have a static global mapping stream to the
  // CUDAGraph. Using that, I can build the hashmap inside of this
  // CUDAGraph!

class MemHandleHolder {
public:
    ~MemHandleHolder() {
        if (handle != 0) {
            // Release the handle when the static object is destroyed
            c10::cuda::DriverAPI::get()->cuMemRelease_(handle);
        }
    }
    
    CUmemGenericAllocationHandle handle{0};
};

void* DynamicCUDAGraphMemoryAllocator::cudagraph_malloc(size_t size, int device, cudaStream_t stream) {
  std::cout << "GALVEZ:cudagraph_malloc" << std::endl;
  at::cuda::OptionalCUDAGuard gpuGuard(device);

    static MemHandleHolder handleHolder;
    static std::once_flag initFlag;
    
    std::call_once(initFlag, [&]() {
      CUmemAllocationProp prop{};
      prop.type = CU_MEM_ALLOCATION_TYPE_PINNED;
      prop.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
      prop.location.id = device;
      C10_CUDA_DRIVER_CHECK(c10::cuda::DriverAPI::get()->cuMemCreate_(&handleHolder.handle, TWO_MIB, &prop, 0ULL));
    });

    CUmemGenericAllocationHandle &handle = handleHolder.handle;

    void *ptr;
    size_t size_rounded_up_to_page = roundUpToNearestTwoMiB(size);
    C10_CUDA_DRIVER_CHECK(c10::cuda::DriverAPI::get()->cuMemAddressReserve_((CUdeviceptr*)&ptr, size_rounded_up_to_page, 0ULL, 0, 0ULL));

    CUmemAccessDesc desc{};
    desc.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    // NOLINTNEXTLINE(bugprone-signed-char-misuse)
    desc.location.id = device;
    // we are marking this page inaccessible because we should never
    // actually access it. It should always be replaced by parameterized
    // graph launch
    desc.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
    // TODO: Does this make my type no longer a memobj, but instead a
    // VA? possibly... But the more likely culprit is that I am
    // unmapping this memory in become_dynamic()... Wait... but I'm not doing that...
    for (uintptr_t addr = (uintptr_t)ptr; addr != ((uintptr_t)ptr) + size_rounded_up_to_page; addr += TWO_MIB) {
      C10_CUDA_DRIVER_CHECK(c10::cuda::DriverAPI::get()->cuMemMap_((CUdeviceptr)(addr), TWO_MIB, 0, handle, 0ULL));
      // C10_CUDA_DRIVER_CHECK(c10::cuda::DriverAPI::get()->cuMemSetAccess_((CUdeviceptr)addr, TWO_MIB /* can I use size? */, &desc, 1));
    }

    C10_CUDA_DRIVER_CHECK(c10::cuda::DriverAPI::get()->cuMemSetAccess_((CUdeviceptr)ptr, size_rounded_up_to_page /* can I use size? */, &desc, 1));

    // 0x20 0000 is minimum granularity


    cudaPointerAttributes attributes;

    AT_CUDA_CHECK(cudaPointerGetAttributes(&attributes, ptr));

    std::cout << "GALVEZ: cudaPointerGetAttributes device pointer " << attributes.devicePointer << " host pointer " << attributes.hostPointer << std::endl;

    graph_->unbacked_memory.emplace_back(CUDAGraph::AllocationMetadata{ptr, size});
    return ptr;
  }

void DynamicCUDAGraphMemoryAllocator::cudagraph_free(void *ptr, size_t size, int device, cudaStream_t stream) {
  // I should really get a backtrace here to see what is causing this.
  std::cout << "GALVEZ:cudagraph_free" << std::endl;
  at::cuda::OptionalCUDAGuard gpuGuard(device);

  cudaStreamCaptureStatus status{};
  CaptureId_t capture_id{};
  AT_CUDA_CHECK(cudaStreamGetCaptureInfo(stream, &status, &capture_id));
  TORCH_INTERNAL_ASSERT(status != cudaStreamCaptureStatus::cudaStreamCaptureStatusActive, "I'm not sure why a block would ever be freed during stream capture...");
  
  size_t size_rounded_up_to_page = roundUpToNearestTwoMiB(size);
  CUmemGenericAllocationHandle handle{};
  C10_CUDA_DRIVER_CHECK(c10::cuda::DriverAPI::get()->cuMemRetainAllocationHandle_(&handle, ptr));

  bool ptr_is_backed = true;
  for (auto&&[ptr_check, _]: graph_->unbacked_memory) {
    if(ptr == ptr_check) {
      ptr_is_backed = false;
    }
    break;
  }

  if (ptr_is_backed) {
    // in this case, we want to deallocate the single physical memory
    // handle backing the entirety of this virtual memory segment
    C10_CUDA_DRIVER_CHECK(c10::cuda::DriverAPI::get()->cuMemRetainAllocationHandle_(&handle, ptr)); // not a CUdeviceptr here!
    C10_CUDA_DRIVER_CHECK(c10::cuda::DriverAPI::get()->cuMemRelease_(handle));
  } else {
    // do nothing. The static destructor will delete the 2 MiB
    // physical block.
  }

  C10_CUDA_DRIVER_CHECK(c10::cuda::DriverAPI::get()->cuMemUnmap_((CUdeviceptr)ptr, size_rounded_up_to_page));
  // Remember, this should be called *only* after we have destroyed the owning CUDAGraph.
  // Maybe we will one day change to invariant to not delete the virtual memory
  // (only the physical memory). It's a TODO.
  // C10_CUDA_DRIVER_CHECK(c10::cuda::DriverAPI::get()->cuMemAddressFree_((CUdeviceptr)ptr, size_rounded_up_to_page));
}

// TODO: Maybe we can remove this entry in CUDAGraph::reset()?
// Or maybe the CUDAGraph can have a std::shared_ptr<c10::cuda::CUDACachingAllocator::CUDAAllocator> member...
std::vector<std::shared_ptr<c10::cuda::CUDACachingAllocator::CUDAAllocator>> allocators;

// Create a `CUDAPluggableAllocator` that uses the above functions.
std::shared_ptr<c10::Allocator> CUDAGraph::get_mem_allocator() {
  C10_LOG_API_USAGE_ONCE("CUDAGraph.getMemAllocator");
  this->allocator_ = std::make_unique<DynamicCUDAGraphMemoryAllocator>(this);
  // we need to make sure that this allocator remains live until all
  // tensors are destroyed. That is is a bit challenging...
  // how is this kept alive?
  std::shared_ptr<c10::cuda::CUDACachingAllocator::CUDAAllocator>
    pluggable_allocator =
    torch::cuda::CUDAPluggableAllocator::createCustomAllocator(
                                                               std::bind(&DynamicCUDAGraphMemoryAllocator::cudagraph_malloc, this->allocator_.get(), std::placeholders::_1, std::placeholders::_2, std::placeholders::_3),
                                                               std::bind(&DynamicCUDAGraphMemoryAllocator::cudagraph_free, this->allocator_.get(), std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4));
  allocators.push_back(pluggable_allocator);
  return pluggable_allocator;
}

ska::flat_hash_map<void*, CUDAGraph*> CUDAGraph::memory_pointer_to_graph;

} // namespace at::cuda
