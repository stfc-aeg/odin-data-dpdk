import ctypes
import os
import numpy as np
import time

# Frame size settings
FRAME_OUTER_CHUNK_SIZE = 10
PACKETS_PER_FRAME = 250
PAYLOAD_SIZE = 8000
BIT_DEPTH = 16

pixel_length =int((PAYLOAD_SIZE / 2) * PACKETS_PER_FRAME)

# Load the DPDK wrapper library
lib_path = os.path.join(os.path.dirname(__file__), 'lib/libdpdk_wrapper.so')
try:
    lib = ctypes.CDLL(lib_path)
except OSError as e:
    raise RuntimeError(f"Failed to load DPDK wrapper library from '{lib_path}': {e}")

# Define function prototypes
lib.wrapper_rte_eal_init.restype = ctypes.c_int
lib.wrapper_rte_eal_init.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_char_p)]

lib.wrapper_rte_ring_lookup.restype = ctypes.c_void_p
lib.wrapper_rte_ring_lookup.argtypes = [ctypes.c_char_p]

lib.wrapper_rte_ring_dequeue.restype = ctypes.c_int
lib.wrapper_rte_ring_dequeue.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]

lib.wrapper_rte_ring_enqueue.restype = ctypes.c_int
lib.wrapper_rte_ring_enqueue.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

lib.wrapper_rte_eal_primary_proc_alive.restype = ctypes.c_int
lib.wrapper_rte_eal_primary_proc_alive.argtypes = [ctypes.c_char_p]

# Define static Frame structure
class SuperFrameHeader(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("super_image_number",          ctypes.c_uint64),
        ("images_received",             ctypes.c_uint32),
        ("super_image_start_time",      ctypes.c_uint64),
        ("super_image_complete_time",   ctypes.c_uint64),
        ("super_image_time_delta",      ctypes.c_uint64),
        ("super_image_image_size",      ctypes.c_uint64),
        ("super_image_size",            ctypes.c_uint64),
        ("frame_state",                 ctypes.c_uint8 * FRAME_OUTER_CHUNK_SIZE)
    ]

class FrameHeader(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("image_number",                ctypes.c_uint64),
        ("packets_received",            ctypes.c_uint32),
        ("sof_marker_count",            ctypes.c_uint32),
        ("eof_marker_count",            ctypes.c_uint32),
        ("image_start_time",            ctypes.c_uint64),
        ("image_complete_time",         ctypes.c_uint64),
        ("image_time_delta",            ctypes.c_uint32),
        ("image_size",                  ctypes.c_uint64),
        ("packet_state",                ctypes.c_uint8 * PACKETS_PER_FRAME)
    ]

class FrameData(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("pixel_data", ctypes.c_uint16 * (pixel_length))
    ]

class Frame(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("super_header", SuperFrameHeader),
        ("frame_headers", FrameHeader * FRAME_OUTER_CHUNK_SIZE),
        ("frame_data", FrameData * FRAME_OUTER_CHUNK_SIZE)
    ]

    def get_numpy_pixel_data(self, frame_index):
        if frame_index < 0 or frame_index >= FRAME_OUTER_CHUNK_SIZE:
            raise IndexError("Frame index out of range")

        # Convert the ctypes array to a NumPy array
        return np.ctypeslib.as_array(self.frame_data[frame_index].pixel_data)

    @classmethod
    def from_pointer(cls, ptr):
        return ctypes.cast(ptr, ctypes.POINTER(cls)).contents

    def as_pointer(self):
        return ctypes.pointer(self)

class Ring:
    def __init__(self, name):
        self.name = name
        self.ring_ptr = lib.wrapper_rte_ring_lookup(name.encode('utf-8'))
        if not self.ring_ptr:
            raise ValueError(f"Ring '{name}' not found")

    def dequeue(self):
        frame_ptr = ctypes.c_void_p()
        ret = lib.wrapper_rte_ring_dequeue(self.ring_ptr, ctypes.byref(frame_ptr))
        if ret == 0:
            return Frame.from_pointer(frame_ptr)
        return None

    def enqueue(self, frame):
        ret = lib.wrapper_rte_ring_enqueue(self.ring_ptr, frame.as_pointer())
        return ret == 0


def connect_to_process(prefix):
    args = ['odin-data', f'--file-prefix={prefix}', '--proc-type=secondary']
    argc = len(args)
    argv = (ctypes.c_char_p * argc)(*[arg.encode('utf-8') for arg in args])
    ret = lib.wrapper_rte_eal_init(argc, argv)
    return ret >= 0

# Use None to send NULL, unless the main odin-data dpdk processes is using a config file
def is_primary_process_alive(config_file_path=None):
    if config_file_path is None:
        config_file_path = ctypes.c_char_p()
    else:
        config_file_path = config_file_path.encode('utf-8')
    
    return lib.wrapper_rte_eal_primary_proc_alive(config_file_path) == 1


if __name__ == "__main__":
    if connect_to_process("odin-data"):
        print("Connected to DPDK process")

        # Connect to rings
        clear_frames_ring = Ring("clear_frames_0")
        frame_builder_ring_00_0 = Ring("FrameBuilderCore_00_0")

        frame_number = 0
        while is_primary_process_alive():
            # Dequeue a frame
            frame = clear_frames_ring.dequeue()
            if frame:
                print(f"Dequeued super frame with number: {frame.super_header.super_image_number}")

                for i in range(FRAME_OUTER_CHUNK_SIZE):
                    print(f"Frame {i} image number: {frame.frame_headers[i].image_number}")
                    pixel_data = frame.get_numpy_pixel_data(i)
                    print(f"Frame {i} pixel data shape: {pixel_data.shape}")
                    for j in range(128):
                        pixel_data[j] = j

                # Modify frame data
                frame.super_header.super_image_number = frame_number
                frame_number += 1

                # Enqueue the modified frame
                if frame_builder_ring_00_0.enqueue(frame):
                    print(f"Enqueued super frame with number: {frame.super_header.super_image_number}")
                else:
                    print("Failed to enqueue frame")
            else:
                print("No frame dequeued")

            time.sleep(0.5)

        print("Primary process terminated")
    else:
        print("Failed to connect to DPDK process")