import ctypes
import os
import logging
from typing import Dict, Optional

from ..frame_source import FrameSource
from ..ring import RingInterface
from ..frame_config import FrameConfig
from ..frames import create_frame_classes

logger = logging.getLogger("odin_data_dpdk.dpdk_source")

class DPDKRing(RingInterface):
    """Implementation of a real DPDK ring"""
    
    def __init__(self, name: str, ring_ptr, frame_class):
        self._name = name
        self.ring_ptr = ring_ptr
        self.Frame = frame_class
        self.enqueue_count = 0
        self.dequeue_count = 0
    
    @property
    def name(self) -> str:
        return self._name
    
    def dequeue(self):
        frame_ptr = ctypes.c_void_p()
        ret = lib.wrapper_rte_ring_dequeue(self.ring_ptr, ctypes.byref(frame_ptr))
        if ret == 0:
            self.dequeue_count += 1
            return self.Frame.from_pointer(frame_ptr)
        return None

    def enqueue(self, frame):
        ret = lib.wrapper_rte_ring_enqueue(self.ring_ptr, frame.as_pointer())
        success = ret == 0
        if success:
            self.enqueue_count += 1
        return success
        
    def get_stats(self):
        stats = super().get_stats()
        stats.update({
            "enqueue_count": self.enqueue_count,
            "dequeue_count": self.dequeue_count
        })
        return stats

class DPDKSource(FrameSource):
    """Real DPDK implementation of FrameSource"""
    
    def __init__(self, prefix: str = "odin-data", 
                 config_file_path: Optional[str] = None,
                 frame_config: Optional[FrameConfig] = None):
        """
        Initialize a real DPDK frame source
        
        Args:
            prefix: DPDK file prefix
            config_file_path: Path to config file
            frame_config: Frame configuration
        """
        self.prefix = prefix
        self.config_file_path = config_file_path
        self.frame_config = frame_config or FrameConfig()
        self.connected = False
        self.rings: Dict[str, DPDKRing] = {}
        
        # Create frame types based on configuration
        self.Frame, _, _, _ = create_frame_classes(self.frame_config)
        
        # Load the DPDK library
        self._load_library()
        
    def _load_library(self):
        """Load the DPDK wrapper library"""
        # Get the path to the shared library
        _here = os.path.dirname(os.path.abspath(__file__))
        lib_path = os.path.join(os.path.dirname(os.path.dirname(_here)), 'lib', 'libdpdk_wrapper.so')
        
        try:
            global lib
            lib = ctypes.CDLL(lib_path)
            self._setup_function_prototypes()
            logger.debug(f"Successfully loaded DPDK wrapper library from '{lib_path}'")
        except OSError as e:
            logger.error(f"Failed to load DPDK wrapper library from '{lib_path}': {e}")
            raise ValueError(f"Failed to load DPDK wrapper library: {e}")
    
    def _setup_function_prototypes(self):
        """Define function prototypes for the DPDK library"""
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
    
    def connect(self):
        """Connect to the DPDK process"""
        if self.connected:
            return True
            
        args = ['odin-data', f'--file-prefix={self.prefix}', '--proc-type=secondary']
        argc = len(args)
        argv = (ctypes.c_char_p * argc)(*[arg.encode('utf-8') for arg in args])
        
        ret = lib.wrapper_rte_eal_init(argc, argv)
        self.connected = ret >= 0
        
        if self.connected:
            logger.info(f"Successfully connected to DPDK process with prefix '{self.prefix}'")
        else:
            logger.error(f"Failed to connect to DPDK process with prefix '{self.prefix}'")
            
        return self.connected
    
    def is_connected(self):
        return self.connected
    
    def is_active(self):
        """Check if the primary DPDK process is alive"""
        if not self.connected:
            logger.warning("Cannot check primary process status: not connected")
            return False
            
        config_path = None
        if self.config_file_path:
            config_path = self.config_file_path.encode('utf-8')
        
        return lib.wrapper_rte_eal_primary_proc_alive(config_path) == 1
    
    def create_ring(self, name):
        """Create a ring interface for the given name"""
        if not self.connected:
            raise ValueError("Not connected to DPDK process")
            
        if name in self.rings:
            return self.rings[name]
            
        ring_ptr = lib.wrapper_rte_ring_lookup(name.encode('utf-8'))
        if not ring_ptr:
            logger.error(f"Ring '{name}' not found")
            raise ValueError(f"Ring '{name}' not found")
            
        ring = DPDKRing(name, ring_ptr, self.Frame)
        self.rings[name] = ring
        logger.info(f"Connected to ring '{name}'")
        return ring
    
    def shutdown(self):
        """Clean up resources"""
        # Currently no specific cleanup needed for real backend
        self.connected = False
        logger.info("DPDK source shut down")