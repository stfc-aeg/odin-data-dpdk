from abc import ABC, abstractmethod
from typing import Optional

class FrameSource(ABC):
    """Abstract base class for frame data sources"""
    
    @abstractmethod
    def connect(self) -> bool:
        """Establish connection to the frame source
        
        Returns:
            bool: True if connection successful, False otherwise
        """
        pass
        
    @abstractmethod
    def is_connected(self) -> bool:
        """Check if source is connected
        
        Returns:
            bool: True if connected, False otherwise
        """
        pass
        
    @abstractmethod
    def is_active(self) -> bool:
        """Check if the source is active and can provide frames
        
        For DPDK, this checks if primary process is alive.
        For simulations, this controls whether simulation continues.
        
        Returns:
            bool: True if source is active, False otherwise
        """
        pass
        
    @abstractmethod
    def create_ring(self, name: str) -> 'RingInterface':
        """Create a ring interface for the given name
        
        Args:
            name: Name of the ring
            
        Returns:
            RingInterface: An interface to interact with the ring
            
        Raises:
            ValueError: If ring creation fails
        """
        pass
        
    @abstractmethod
    def shutdown(self) -> None:
        """Clean up resources used by the source"""
        pass