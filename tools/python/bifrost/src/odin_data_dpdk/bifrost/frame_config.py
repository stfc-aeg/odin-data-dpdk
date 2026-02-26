from dataclasses import dataclass
from typing import Optional

@dataclass
class FrameConfig:
    """Configuration for DPDK frame structures"""
    frame_outer_chunk_size: int = 1
    packets_per_frame: int = 1
    payload_size: int = 18874368
    bit_depth: int = 16
    
    @property
    def pixel_length(self) -> int:
        """Calculate pixel length based on configuration"""
        bytes_per_pixel = self.bit_depth / 8
        return int((self.payload_size / bytes_per_pixel) * self.packets_per_frame)