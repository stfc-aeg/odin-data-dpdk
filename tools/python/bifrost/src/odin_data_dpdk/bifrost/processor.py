import time
import logging
from typing import Optional, Callable, Dict, Any

from .frame_config import FrameConfig
from .sources import get_source_class

logger = logging.getLogger("odin_data_dpdk.processor")

class DPDKProcessor:
    """Main processor for DPDK frames"""
    
    def __init__(self, 
                 source_type: str = "dpdk",
                 source_config: Optional[Dict[str, Any]] = None,
                 frame_config: Optional[FrameConfig] = None):
        """
        Initialize the DPDK processor with the specified frame source
        
        Args:
            source_type: Type of frame source to use ("dpdk", "generated", "hdf5")
            source_config: Configuration for the frame source
            frame_config: Configuration for frame structure
        """
        self.source_type = source_type
        self.source_config = source_config or {}
        self.frame_config = frame_config or FrameConfig()
        
        # Create the appropriate source
        source_class = get_source_class(source_type)
        self.source = source_class(frame_config=self.frame_config, **self.source_config)
        
        # Track rings for easy access
        self.rings = {}
    
    def connect(self) -> bool:
        """Connect to the frame source
        
        Returns:
            bool: True if connection successful, False otherwise
        """
        return self.source.connect()
    
    def is_primary_alive(self) -> bool:
        """Check if the primary process is alive
        
        Returns:
            bool: True if primary process is alive, False otherwise
        """
        return self.source.is_active()
    
    def connect_to_ring(self, ring_name: str):
        """Connect to a ring
        
        Args:
            ring_name: Name of the ring
            
        Returns:
            RingInterface: Interface to interact with the ring
            
        Raises:
            ValueError: If not connected or ring creation fails
        """
        if not self.source.is_connected():
            raise ValueError("Not connected to frame source")
            
        if ring_name in self.rings:
            return self.rings[ring_name]
            
        ring = self.source.create_ring(ring_name)
        self.rings[ring_name] = ring
        return ring
    
    def process_frames(self, 
                    source_ring_name: str = "PythonRingBuffer_00_0", 
                    dest_ring_name: str = "PythonAccessCore_00_0", 
                    max_frames: Optional[int] = None, 
                    process_func: Optional[Callable] = None,
                    stats_interval: int = 100):
        """
        Process frames from source ring and send to destination ring
        
        Args:
            source_ring_name: Name of source ring
            dest_ring_name: Name of destination ring
            max_frames: Maximum number of frames to process
            process_func: Function to process each frame
            stats_interval: How often to log statistics (frames)
            
        Returns:
            int: Number of frames processed
            
        Raises:
            ValueError: If not connected
        """
        if not self.source.is_connected():
            raise ValueError("Not connected to frame source")
            
        source_ring = self.connect_to_ring(source_ring_name)
        dest_ring = self.connect_to_ring(dest_ring_name)
        
        frame_count = 0
        start_time = time.time()
        
        try:
            while self.is_primary_alive() and (max_frames is None or frame_count < max_frames):
                # Dequeue a frame
                frame = source_ring.dequeue()
                if frame:
                    frame_count += 1
                    
                    # Process frame if a processing function is provided
                    if process_func:
                        frame = process_func(frame)
                    
                    # Enqueue the frame
                    if not dest_ring.enqueue(frame):
                        logger.warning(f"Failed to enqueue frame {frame_count}")
                    
                    # Log progress periodically
                    if frame_count % stats_interval == 0:
                        elapsed = time.time() - start_time
                        rate = frame_count / elapsed if elapsed > 0 else 0
                        logger.info(f"Processed {frame_count} frames ({rate:.2f} frames/s)")
                else:
                    # Short sleep when no frames are available to prevent CPU hogging
                    time.sleep(0.001)
        finally:
            # Ensure we log final stats
            elapsed = time.time() - start_time
            rate = frame_count / elapsed if elapsed > 0 else 0
            logger.info(f"Finished processing {frame_count} frames in {elapsed:.2f}s ({rate:.2f} frames/s)")
            
            # Get and log ring statistics
            source_stats = source_ring.get_stats()
            dest_stats = dest_ring.get_stats()
            logger.info(f"Source ring stats: {source_stats}")
            logger.info(f"Destination ring stats: {dest_stats}")
        
        return frame_count
    
    def shutdown(self):
        """Shut down the processor and source"""
        self.source.shutdown()