from .dpdk_source import DPDKSource

# Conditionally import HDF5Source if h5py is available
try:
    import h5py
    from .hdf5_source import HDF5Source
    HAS_H5PY = True
except ImportError:
    HAS_H5PY = False

# Registry of available sources
SOURCES = {
    "dpdk": DPDKSource
}

if HAS_H5PY:
    SOURCES["hdf5"] = HDF5Source

def get_source_class(source_type):
    """Get the class for a source type"""
    if source_type not in SOURCES:
        if source_type == "hdf5" and not HAS_H5PY:
            raise ImportError("HDF5 source requires h5py package which is not installed")
        raise ValueError(f"Unknown source type: {source_type}")
    
    return SOURCES[source_type]

__all__ = ["DPDKSource", "get_source_class"]
if HAS_H5PY:
    __all__.append("HDF5Source")