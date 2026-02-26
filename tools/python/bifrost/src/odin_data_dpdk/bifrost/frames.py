import ctypes
import numpy as np
from typing import Tuple, Type
from .frame_config import FrameConfig

def create_frame_classes(config: FrameConfig) -> Tuple[Type, Type, Type, Type]:
    """Create frame structure classes based on configuration
    
    Args:
        config: Frame configuration parameters
        
    Returns:
        Tuple containing (Frame, FrameHeader, SuperFrameHeader, FrameData) classes
    """
    
    class SuperFrameHeader(ctypes.Structure):
        _pack_ = 1
        _fields_ = [
            ("super_image_number",        ctypes.c_uint64),
            ("images_received",           ctypes.c_uint32),
            ("super_image_start_time",    ctypes.c_uint64),
            ("super_image_complete_time", ctypes.c_uint64),
            ("super_image_time_delta",    ctypes.c_uint64),
            ("super_image_image_size",    ctypes.c_uint64),
            ("super_image_size",          ctypes.c_uint64),
            ("frame_state",               ctypes.c_uint8 * config.frame_outer_chunk_size)
        ]

    class FrameHeader(ctypes.Structure):
        _pack_ = 1
        _fields_ = [
            ("image_number",              ctypes.c_uint64),
            ("packets_received",          ctypes.c_uint32),
            ("sof_marker_count",          ctypes.c_uint32),
            ("eof_marker_count",          ctypes.c_uint32),
            ("image_start_time",          ctypes.c_uint64),
            ("image_complete_time",       ctypes.c_uint64),
            ("image_time_delta",          ctypes.c_uint32),
            ("image_size",                ctypes.c_uint64),
            ("packet_state",              ctypes.c_uint8 * config.packets_per_frame)
        ]

    class FrameData(ctypes.Structure):
        _pack_ = 1
        _fields_ = [
            ("pixel_data", ctypes.c_uint16 * config.pixel_length)
        ]

    class Frame(ctypes.Structure):
        _pack_ = 1
        _fields_ = [
            ("super_header", SuperFrameHeader),
            ("frame_headers", FrameHeader * config.frame_outer_chunk_size),
            ("frame_data", FrameData * config.frame_outer_chunk_size)
        ]
        
        def get_numpy_pixel_data(self):
            """Get frame data as a NumPy array"""
            return np.ctypeslib.as_array(self.frame_data[0].pixel_data)

        @classmethod
        def from_pointer(cls, ptr):
            """Create Frame from pointer"""
            if not ptr:
                raise ValueError("Cannot create Frame from NULL pointer")
            return ctypes.cast(ptr, ctypes.POINTER(cls)).contents

        def as_pointer(self):
            """Get pointer to this frame"""
            return ctypes.pointer(self)
    
    return Frame, FrameHeader, SuperFrameHeader, FrameData