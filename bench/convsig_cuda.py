"""CUDA driver API + NVRTC binding for the TER^2 convolution/source-signature spike.

SCOPE.  Research tooling only; nothing here is linked into or used by production.
This machine has the NVIDIA driver (nvcuda.dll) but no CUDA toolkit, so kernels are
compiled at runtime with NVRTC taken from the pinned wheel
nvidia-cuda-nvrtc==13.1.115, whose directory is passed explicitly.

FAIL-CLOSED.  Every driver and NVRTC call is checked.  Any failure raises
CudaError.  Callers must treat CudaError as MAYBE/FALLBACK, never as an empty
result.

The driver's legacy v1 memory entry points reject contexts created through the
current context API with CUDA_ERROR_INVALID_CONTEXT, so the size_t *_v2 entry
points are preferred whenever the driver exports them.
"""
from __future__ import annotations

import ctypes as C
import glob
import os
import time

_P, _I, _U, _S = C.c_void_p, C.c_int, C.c_uint, C.c_size_t


class CudaError(RuntimeError):
    """Any driver, NVRTC, geometry, or resource failure.  Never an empty result."""


def driver_candidates() -> list[str]:
    root = os.environ.get("SystemRoot", r"C:\Windows")
    paths = [os.path.join(root, "System32", "nvcuda.dll")]
    paths += sorted(glob.glob(os.path.join(root, "System32", "DriverStore", "FileRepository",
                                           "nv*", "nvcuda64.dll")))
    return [path for path in paths if os.path.isfile(path)]


_DRIVER_SIGNATURES = {
    "cuInit": [_U],
    "cuDriverGetVersion": [C.POINTER(_I)],
    "cuDeviceGet": [C.POINTER(_I), _I],
    "cuDeviceGetName": [C.c_char_p, _I, _I],
    "cuDeviceComputeCapability": [C.POINTER(_I), C.POINTER(_I), _I],
    "cuDeviceTotalMem": [C.POINTER(_S), _I],
    "cuDeviceTotalMem_v2": [C.POINTER(_S), _I],
    "cuDevicePrimaryCtxRetain": [C.POINTER(_P), _I],
    "cuDevicePrimaryCtxRelease": [_I],
    "cuDevicePrimaryCtxRelease_v2": [_I],
    "cuCtxCreate_v2": [C.POINTER(_P), _U, _I],
    "cuCtxDestroy_v2": [_P],
    "cuCtxSetCurrent": [_P],
    "cuCtxGetCurrent": [C.POINTER(_P)],
    "cuCtxSynchronize": [],
    "cuMemGetInfo_v2": [C.POINTER(_S), C.POINTER(_S)],
    "cuMemGetInfo": [C.POINTER(_S), C.POINTER(_S)],
    "cuMemAlloc_v2": [C.POINTER(_P), _S],
    "cuMemAlloc": [C.POINTER(_P), _S],
    "cuMemFree_v2": [_P],
    "cuMemFree": [_P],
    "cuMemAllocHost_v2": [C.POINTER(_P), _S],
    "cuMemFreeHost": [_P],
    "cuMemcpyHtoD_v2": [_P, _P, _S],
    "cuMemcpyHtoD": [_P, _P, _S],
    "cuMemcpyDtoH_v2": [_P, _P, _S],
    "cuMemcpyDtoH": [_P, _P, _S],
    "cuModuleLoadData": [C.POINTER(_P), C.c_char_p],
    "cuModuleGetFunction": [C.POINTER(_P), _P, C.c_char_p],
    "cuLaunchKernel": [_P, _U, _U, _U, _U, _U, _U, _U, _P, C.POINTER(_P), C.POINTER(_P)],
    "cuGetErrorName": [_I, C.POINTER(C.c_char_p)],
}

_NVRTC_SIGNATURES = {
    "nvrtcVersion": [C.POINTER(_I), C.POINTER(_I)],
    "nvrtcCreateProgram": [C.POINTER(_P), C.c_char_p, C.c_char_p, _I, _P, _P],
    "nvrtcCompileProgram": [_P, _I, C.POINTER(C.c_char_p)],
    "nvrtcGetProgramLogSize": [_P, C.POINTER(_S)],
    "nvrtcGetProgramLog": [_P, C.c_char_p],
    "nvrtcGetCUBINSize": [_P, C.POINTER(_S)],
    "nvrtcGetCUBIN": [_P, C.c_char_p],
    "nvrtcDestroyProgram": [C.POINTER(_P)],
}


class _Library:
    def __init__(self, path: str, signatures: dict):
        self.path = path
        self.dll = C.CDLL(path)
        self.available = set()
        for name, args in signatures.items():
            try:
                function = getattr(self.dll, name)
            except AttributeError:
                continue
            function.argtypes = args
            function.restype = _I
            self.available.add(name)

    def pick(self, *names):
        for name in names:
            if name in self.available:
                return name, getattr(self.dll, name)
        raise CudaError(f"none of {names} exported by {self.path}")


class Gpu:
    """One device, one context, explicit ownership of every allocation."""

    def __init__(self, nvrtc_dir: str, block_size: int = 1024):
        if not nvrtc_dir or not os.path.isdir(nvrtc_dir):
            raise CudaError(f"NVRTC directory not found: {nvrtc_dir!r}")
        self.block_size = block_size
        self.attempts: list[tuple[str, str, str]] = []
        self.allocations: dict[int, int] = {}
        self.host_allocations: dict[int, int] = {}
        self.peak_device_bytes = 0
        self.launches = 0
        opened = False
        start = time.perf_counter()
        for path in driver_candidates():
            for strategy in ("primary", "create_v2"):
                try:
                    self._open(path, strategy)
                    self.attempts.append((path, strategy, "ok"))
                    opened = True
                    break
                except (CudaError, OSError) as error:
                    self.attempts.append((path, strategy, str(error)))
            if opened:
                break
        if not opened:
            raise CudaError(f"no working CUDA context; attempts={self.attempts!r}")
        self.context_ms = (time.perf_counter() - start) * 1e3
        self._load_nvrtc(nvrtc_dir)

    # -- setup -------------------------------------------------------------
    def _check(self, code: int, what: str) -> None:
        if code != 0:
            name = C.c_char_p()
            if "cuGetErrorName" in self.driver.available:
                self.driver.dll.cuGetErrorName(code, C.byref(name))
            raise CudaError(f"{what} failed: {code} {name.value!r}")

    def _open(self, path: str, strategy: str) -> None:
        self.driver = _Library(path, _DRIVER_SIGNATURES)
        dll = self.driver.dll
        self._check(dll.cuInit(0), "cuInit")
        device = _I()
        self._check(dll.cuDeviceGet(C.byref(device), 0), "cuDeviceGet")
        self.device = device.value
        context = _P()
        if strategy == "primary":
            self._check(dll.cuDevicePrimaryCtxRetain(C.byref(context), self.device),
                        "cuDevicePrimaryCtxRetain")
            self._check(dll.cuCtxSetCurrent(context), "cuCtxSetCurrent")
        else:
            if "cuCtxCreate_v2" not in self.driver.available:
                raise CudaError("cuCtxCreate_v2 not exported")
            self._check(dll.cuCtxCreate_v2(C.byref(context), 0, self.device), "cuCtxCreate_v2")
        self.context = context.value
        self.context_strategy = strategy
        self.memory_api, self._mem_get_info = self.driver.pick("cuMemGetInfo_v2", "cuMemGetInfo")
        _, self._mem_alloc = self.driver.pick("cuMemAlloc_v2", "cuMemAlloc")
        _, self._mem_free = self.driver.pick("cuMemFree_v2", "cuMemFree")
        _, self._htod = self.driver.pick("cuMemcpyHtoD_v2", "cuMemcpyHtoD")
        _, self._dtoh = self.driver.pick("cuMemcpyDtoH_v2", "cuMemcpyDtoH")
        free, total = self.memory_info()
        probe = self.alloc(1 << 20)
        self.free(probe)
        self.initial_free_bytes = free
        self.total_bytes = total

    def _load_nvrtc(self, directory: str) -> None:
        os.add_dll_directory(directory)
        # NVRTC loads its builtins library by bare name through the default DLL
        # search order, which ignores add_dll_directory.  Preloading it by full
        # path and exposing the directory on PATH makes that lookup deterministic.
        os.environ["PATH"] = directory + os.pathsep + os.environ.get("PATH", "")
        self.builtins = []
        for name in sorted(os.listdir(directory)):
            if name.startswith("nvrtc-builtins") and name.endswith(".dll"):
                self.builtins.append(C.CDLL(os.path.join(directory, name)))
        names = sorted(name for name in os.listdir(directory)
                       if name.startswith("nvrtc64_") and name.endswith(".dll"))
        if not names:
            raise CudaError(f"no nvrtc64_*.dll in {directory}")
        self.nvrtc = _Library(os.path.join(directory, names[0]), _NVRTC_SIGNATURES)
        major, minor = _I(), _I()
        self.nvrtc.dll.nvrtcVersion(C.byref(major), C.byref(minor))
        self.nvrtc_version = f"{major.value}.{minor.value}"
        self.nvrtc_path = self.nvrtc.path

    # -- properties --------------------------------------------------------
    def properties(self) -> dict:
        dll = self.driver.dll
        name = C.create_string_buffer(256)
        self._check(dll.cuDeviceGetName(name, 256, self.device), "cuDeviceGetName")
        major, minor = _I(), _I()
        self._check(dll.cuDeviceComputeCapability(C.byref(major), C.byref(minor), self.device),
                    "cuDeviceComputeCapability")
        driver = _I()
        self._check(dll.cuDriverGetVersion(C.byref(driver)), "cuDriverGetVersion")
        return {
            "name": name.value.decode(errors="replace"),
            "compute_capability": f"{major.value}.{minor.value}",
            "arch_flag": f"sm_{major.value}{minor.value}",
            "total_memory_bytes": self.total_bytes,
            "free_memory_bytes_at_open": self.initial_free_bytes,
            "driver_api_version": driver.value,
            "driver_dll": self.driver.path,
            "context_strategy": self.context_strategy,
            "memory_api": self.memory_api,
            "nvrtc_version": self.nvrtc_version,
            "nvrtc_dll": self.nvrtc_path,
            "block_size": self.block_size,
            "context_open_ms": self.context_ms,
            "open_attempts": self.attempts,
        }

    # -- memory ------------------------------------------------------------
    def memory_info(self) -> tuple[int, int]:
        free, total = _S(), _S()
        self._check(self._mem_get_info(C.byref(free), C.byref(total)), "cuMemGetInfo")
        return free.value, total.value

    def alloc(self, nbytes: int) -> int:
        pointer = _P()
        self._check(self._mem_alloc(C.byref(pointer), max(int(nbytes), 1)), f"cuMemAlloc({nbytes})")
        self.allocations[pointer.value] = int(nbytes)
        self.peak_device_bytes = max(self.peak_device_bytes, sum(self.allocations.values()))
        return pointer.value

    def free(self, pointer: int) -> None:
        self._check(self._mem_free(_P(pointer)), "cuMemFree")
        self.allocations.pop(pointer, None)

    def device_bytes(self) -> int:
        return sum(self.allocations.values())

    def upload(self, pointer: int, array) -> None:
        self._check(self._htod(_P(pointer), _P(array.ctypes.data), array.nbytes), "cuMemcpyHtoD")

    def download(self, array, pointer: int) -> None:
        self._check(self._dtoh(_P(array.ctypes.data), _P(pointer), array.nbytes), "cuMemcpyDtoH")

    # -- kernels -----------------------------------------------------------
    def compile(self, source: bytes, name: bytes, arch_flag: str) -> tuple[int, dict]:
        dll = self.nvrtc.dll
        program = _P()
        created = dll.nvrtcCreateProgram(C.byref(program), source, name, 0, None, None)
        if created != 0:
            raise CudaError(f"nvrtcCreateProgram failed: {created}")
        options = [f"-arch={arch_flag}".encode(), b"-std=c++17"]
        option_array = (C.c_char_p * len(options))(*options)
        start = time.perf_counter()
        result = dll.nvrtcCompileProgram(program, len(options), option_array)
        compile_ms = (time.perf_counter() - start) * 1e3
        size = _S()
        dll.nvrtcGetProgramLogSize(program, C.byref(size))
        log = C.create_string_buffer(max(size.value, 1))
        dll.nvrtcGetProgramLog(program, log)
        log_text = log.value.decode(errors="replace").strip()
        if result != 0:
            dll.nvrtcDestroyProgram(C.byref(program))
            raise CudaError(f"NVRTC compile failed ({result}): {log_text}")
        size = _S()
        if dll.nvrtcGetCUBINSize(program, C.byref(size)) != 0:
            raise CudaError("nvrtcGetCUBINSize failed")
        image = C.create_string_buffer(size.value)
        if dll.nvrtcGetCUBIN(program, image) != 0:
            raise CudaError("nvrtcGetCUBIN failed")
        dll.nvrtcDestroyProgram(C.byref(program))
        module = _P()
        start = time.perf_counter()
        self._check(self.driver.dll.cuModuleLoadData(C.byref(module), image), "cuModuleLoadData")
        load_ms = (time.perf_counter() - start) * 1e3
        return module.value, {"compile_ms": compile_ms, "module_load_ms": load_ms,
                              "options": [option.decode() for option in options],
                              "image_bytes": size.value, "log": log_text}

    def function(self, module: int, name: bytes) -> int:
        function = _P()
        self._check(self.driver.dll.cuModuleGetFunction(C.byref(function), _P(module), name),
                    f"cuModuleGetFunction({name!r})")
        return function.value

    def launch(self, function: int, total_threads: int, args: list, shared_bytes: int = 0) -> None:
        """Asynchronous launch.  args are ctypes values; the driver copies them."""
        if total_threads <= 0:
            return
        if total_threads >= (1 << 32):
            raise CudaError(f"thread count {total_threads} exceeds the 32-bit kernel index")
        block = self.block_size
        grid = (total_threads + block - 1) // block
        storage = [C.c_void_p(arg) if isinstance(arg, DevicePointer) else arg for arg in args]
        params = (_P * len(storage))(*[C.addressof(value) for value in storage])
        self._check(self.driver.dll.cuLaunchKernel(_P(function), grid, 1, 1, block, 1, 1,
                                                   int(shared_bytes), None, params, None),
                    "cuLaunchKernel")
        self.launches += 1

    def synchronize(self) -> None:
        self._check(self.driver.dll.cuCtxSynchronize(), "cuCtxSynchronize")

    def probe_limits(self, function: int, args: list) -> dict:
        """Empirical per-block shared-memory and threads-per-block limits."""
        shared = {}
        for kib in (16, 32, 48, 49, 64, 128, 1024):
            params = [C.c_void_p(a) if isinstance(a, DevicePointer) else a for a in args]
            array = (_P * len(params))(*[C.addressof(v) for v in params])
            code = self.driver.dll.cuLaunchKernel(_P(function), 1, 1, 1, 16, 1, 1, kib * 1024,
                                                  None, array, None)
            if code == 0:
                code = self.driver.dll.cuCtxSynchronize()
            shared[kib] = code
        threads = {}
        for count in (512, 1024, 2048):
            params = [C.c_void_p(a) if isinstance(a, DevicePointer) else a for a in args]
            array = (_P * len(params))(*[C.addressof(v) for v in params])
            code = self.driver.dll.cuLaunchKernel(_P(function), 1, 1, 1, count, 1, 1, 0,
                                                  None, array, None)
            if code == 0:
                code = self.driver.dll.cuCtxSynchronize()
            threads[count] = code
        return {"shared_mem_bytes_result_by_kib": shared, "threads_per_block_result": threads}


class DevicePointer(int):
    """Marks an int as a device pointer argument for Gpu.launch."""
