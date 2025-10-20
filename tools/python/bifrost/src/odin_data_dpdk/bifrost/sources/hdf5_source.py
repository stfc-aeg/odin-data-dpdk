import time
import logging
import numpy as np
from typing import Dict, Optional

try:
    import h5py
    HAS_H5PY = True
except ImportError:
    HAS_H5PY = False

from ..frame_source import FrameSource
from ..ring import RingInterface
from ..frame_config import FrameConfig
from ..frames import create_frame_classes

logger = logging.getLogger("odin_data_dpdk.hdf5_source")

class HDF5Ring(RingInterface):
    """Simple ring implementation for HDF5-backed frames"""
    
    def __init__(self, name, source):
        self._name = name
        self.source = source
        self.frames_queue = []
        self.enqueue_count = 0
        self.dequeue_count = 0
    
    @property
    def name(self) -> str:
        return self._name
    
    def dequeue(self):
        """Get next frame from HDF5 file or queue"""
        # For source rings, get next frame from source
        if self.name.startswith("source") or not self.frames_queue:
            frame = self.source.get_next_frame()
            if frame:
                self.dequeue_count += 1
            return frame
        
        # For other rings, pop from the queue
        frame = self.frames_queue.pop(0)
        self.dequeue_count += 1
        return frame

    def enqueue(self, frame):
        """Add frame to the queue or save to output"""
        # For destination rings, save to output if configured
        if self.name.startswith("dest") and self.source.output:
            self.source.save_frame(frame)
        
        # Add to queue
        self.frames_queue.append(frame)
        self.enqueue_count += 1
        return True
    
    def get_stats(self):
        stats = super().get_stats()
        stats.update({
            "enqueue_count": self.enqueue_count,
            "dequeue_count": self.dequeue_count,
            "queued_frames": len(self.frames_queue)
        })
        return stats

class HDF5Source(FrameSource):
    """Source that loads frames from HDF5 files"""
    
    def __init__(self, file_path, frame_config=None, 
                 output_file=None, frame_rate=None, loop=False,
                 dataset_path="dummy"):
        """
        Initialize a source that loads frames from HDF5
        
        Args:
            file_path (str): Path to HDF5 file with frames
            frame_config (FrameConfig): Configuration for frame structure
            output_file (str): Optional file to save processed frames
            frame_rate (float): Optional rate limiter (frames per second)
            loop (bool): Whether to loop back to start when reaching end of file
            dataset_path (str): Path to dataset within HDF5 file (default: "dummy")
        """
        if not HAS_H5PY:
            raise ImportError("h5py package is required for HDF5Source but not installed")
            
        self.file_path = file_path
        self.output_file = output_file
        self.frame_rate = frame_rate
        self.loop = loop
        self.dataset_path = dataset_path
        
        self.file = None
        self.output = None
        self.connected = False
        self.rings = {}
        
        # Frame tracking
        self.current_frame_idx = 0
        self.total_frames = 0
        self.frame_shape = None
        self.start_time = None
        
        # Will create frame classes during connect() when we know the dimensions
        self.frame_config = frame_config
        self.Frame = None
    
    def connect(self):
        """Open the HDF5 file and prepare for reading"""
        try:
            # Open input file
            logger.info(f"Opening HDF5 file '{self.file_path}'")
            self.file = h5py.File(self.file_path, 'r')
            
            # Check for dataset
            if self.dataset_path not in self.file:
                logger.error(f"Dataset '{self.dataset_path}' not found in file")
                return False
                
            # Get dataset shape and validate
            dataset = self.file[self.dataset_path]
            if not isinstance(dataset, h5py.Dataset):
                logger.error(f"'{self.dataset_path}' is not a dataset")
                return False
                
            shape = dataset.shape
            if len(shape) < 3:
                logger.error(f"Dataset must be 3D (frames x height x width), got shape {shape}")
                return False
                
            self.total_frames = shape[0]
            self.frame_shape = shape[1:]
            
            logger.info(f"Found {self.total_frames} frames with shape {self.frame_shape}")
            
            # Create frame configuration based on dataset dimensions
            if self.frame_config is None:
                # Calculate appropriate frame config for the given dimensions
                total_pixels = np.prod(self.frame_shape)
                self.frame_config = FrameConfig(
                    frame_outer_chunk_size=1,
                    packets_per_frame=1,
                    payload_size=int(total_pixels * 2),  # 2 bytes per pixel for uint16
                    bit_depth=16
                )
                logger.info(f"Created frame config with payload_size={self.frame_config.payload_size}")
            
            # Create frame classes
            self.Frame, _, _, _ = create_frame_classes(self.frame_config)
            
            # Open output file if specified
            if self.output_file:
                logger.info(f"Creating output file '{self.output_file}'")
                self.output = h5py.File(self.output_file, 'w')
                
                # Create dataset with same structure as input
                self.output.create_dataset(
                    self.dataset_path,
                    shape=(0,) + self.frame_shape,  # Start empty
                    maxshape=(None,) + self.frame_shape,  # Unlimited frames
                    dtype=np.uint16,
                    chunks=(1,) + self.frame_shape  # One frame per chunk
                )
                
                logger.info(f"Created output dataset '{self.dataset_path}' in '{self.output_file}'")
            
            self.connected = True
            self.start_time = time.time()
            return True
            
        except Exception as e:
            logger.error(f"Failed to open HDF5 file: {e}", exc_info=True)
            if self.file:
                self.file.close()
                self.file = None
            return False
    
    def is_connected(self):
        return self.connected
    
    def is_active(self):
        """Check if we should continue providing frames"""
        if not self.connected:
            return False
            
        # If we've read all frames and not looping, we're done
        if self.current_frame_idx >= self.total_frames and not self.loop:
            return False
            
        return True
    
    def create_ring(self, name):
        """Create a simulated ring"""
        if name in self.rings:
            return self.rings[name]
            
        ring = HDF5Ring(name, self)
        self.rings[name] = ring
        logger.info(f"Created HDF5 ring '{name}'")
        return ring
    
    def shutdown(self):
        """Close the HDF5 files"""
        if self.file:
            self.file.close()
            self.file = None
        
        if self.output:
            self.output.close()
            self.output = None
            
        self.connected = False
        logger.info(f"HDF5 source shut down after reading {self.current_frame_idx} frames")
    
    def get_next_frame(self):
        """Get the next frame from the HDF5 file"""
        if not self.connected:
            return None
            
        # Check if we've reached the end
        if self.current_frame_idx >= self.total_frames:
            if self.loop:
                logger.info(f"Looping back to start after {self.total_frames} frames")
                self.current_frame_idx = 0
            else:
                return None
        
        # Rate limiting if frame_rate is specified
        if self.frame_rate and self.current_frame_idx > 0:
            expected_time = self.start_time + (self.current_frame_idx / self.frame_rate)
            current_time = time.time()
            if current_time < expected_time:
                time.sleep(expected_time - current_time)
        
        # Get frame data from HDF5
        try:
            # Load the frame
            frame_data = self.file[self.dataset_path][self.current_frame_idx]
            
            # Create frame and populate
            frame = self.Frame()
            
            # Set basic header info
            frame.super_header.super_image_number = self.current_frame_idx
            frame.super_header.images_received = 1
            frame.frame_headers[0].image_number = self.current_frame_idx
            
            # Copy data to frame
            ctypes_array = frame.frame_data[0].pixel_data
            flat_data = frame_data.flatten()
            data_size = min(len(ctypes_array), flat_data.size)
            
            for i in range(data_size):
                ctypes_array[i] = int(flat_data[i])
            
            # Increment counter
            self.current_frame_idx += 1
            return frame
            
        except Exception as e:
            logger.error(f"Error loading frame {self.current_frame_idx}: {e}")
            self.current_frame_idx += 1
            return None
    
    def save_frame(self, frame):
        """Save a frame to the output file"""
        if not self.output:
            return
            
        try:
            # Get pixel data
            pixel_data = frame.get_numpy_pixel_data()
            
            # Reshape to match original dimensions
            reshaped_data = pixel_data[:np.prod(self.frame_shape)].reshape(self.frame_shape)
            
            # Resize output dataset
            dataset = self.output[self.dataset_path]
            current_size = dataset.shape[0]
            dataset.resize(current_size + 1, axis=0)
            
            # Save frame
            dataset[current_size] = reshaped_data
            
        except Exception as e:
            logger.error(f"Error saving frame: {e}")