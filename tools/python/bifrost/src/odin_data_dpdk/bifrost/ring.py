from abc import ABC, abstractmethod
from typing import Optional, TypeVar, Dict, Any

# Type variable for Frame - allows subclasses to specify exact Frame type
F = TypeVar('F')

class RingInterface(ABC):
    """Abstract interface for ring operations"""
    
    @property
    @abstractmethod
    def name(self) -> str:
        """Get ring name"""
        pass
    
    @abstractmethod
    def dequeue(self) -> Optional[F]:
        """Dequeue a frame from the ring
        
        Returns:
            Optional[Frame]: A frame if available, None otherwise
        """
        pass
        
    @abstractmethod
    def enqueue(self, frame: F) -> bool:
        """Enqueue a frame to the ring
        
        Args:
            frame: Frame to enqueue
            
        Returns:
            bool: True if successful, False otherwise
        """
        pass
        
    def get_stats(self) -> Dict[str, Any]:
        """Get statistics about the ring
        
        Returns:
            dict: Statistics (e.g., enqueue/dequeue counts)
        """
        return {
            "name": self.name,
        }