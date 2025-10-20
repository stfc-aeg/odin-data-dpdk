"""
ODIN Data DPDK Bifrost - DPDK frame processing utilities
"""

from .processor import DPDKProcessor
from .frame_config import FrameConfig
from .frames import create_frame_classes

# Re-export useful classes for easy imports
__all__ = ["DPDKProcessor", "FrameConfig"]