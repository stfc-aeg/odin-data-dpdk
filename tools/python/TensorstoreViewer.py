#!/usr/bin/env python3
"""
TensorStore Dataset Viewer
A simple GUI application for viewing image stacks stored in tensorstore datasets.
"""

import sys
import argparse
import numpy as np
import tensorstore as ts

# IMPORTANT: Set matplotlib backend before importing pyplot
# TensorStore uses internal threading which conflicts with fork-based backends
import matplotlib
matplotlib.use('TkAgg')  # Use Tk backend which doesn't fork

import matplotlib.pyplot as plt
from matplotlib.widgets import Slider, Button, TextBox
from pathlib import Path


class TensorStoreViewer:
    """Interactive viewer for tensorstore image datasets."""
    
    def __init__(self, dataset_path, spec=None, transpose=False):
        """
        Initialize the viewer with a tensorstore dataset.

        Args:
            dataset_path: Path to the tensorstore dataset
            spec: Optional tensorstore spec dict for opening the dataset
            transpose: If True, transpose images (swap height/width)
        """
        self.dataset_path = dataset_path
        self.current_index = 0
        self.transpose = transpose

        # Open the dataset
        self._open_dataset(spec)

        # Setup the GUI
        self._setup_gui()
        
    def _open_dataset(self, spec):
        """Open the tensorstore dataset."""
        try:
            if spec:
                # Use provided spec
                self.dataset = ts.open(spec).result()
            else:
                # Try to auto-detect format by checking for metadata files
                dataset_path = Path(self.dataset_path)
                
                # Check for Zarr v2 (.zarray or .zgroup)
                if (dataset_path / '.zarray').exists() or (dataset_path / '.zgroup').exists():
                    driver = 'zarr'
                    print(f"Detected Zarr v2 format")
                # Check for Zarr v3 (zarr.json)
                elif (dataset_path / 'zarr.json').exists():
                    driver = 'zarr3'
                    print(f"Detected Zarr v3 format")
                # Check for N5 (attributes.json)
                elif (dataset_path / 'attributes.json').exists():
                    driver = 'n5'
                    print(f"Detected N5 format")
                else:
                    # Default fallback - try zarr v2 first
                    driver = 'zarr'
                    print(f"No metadata files detected, trying Zarr v2...")
                
                self.dataset = ts.open({
                    'driver': driver,
                    'kvstore': {
                        'driver': 'file',
                        'path': str(self.dataset_path),
                    },
                }).result()
        except Exception as e:
            print(f"Failed to open with {driver if 'driver' in locals() else 'initial'} driver: {e}")
            # Try fallback formats
            fallback_drivers = ['zarr', 'zarr3', 'n5']
            if 'driver' in locals() and driver in fallback_drivers:
                fallback_drivers.remove(driver)
            
            for fallback_driver in fallback_drivers:
                try:
                    print(f"Trying {fallback_driver} driver...")
                    self.dataset = ts.open({
                        'driver': fallback_driver,
                        'kvstore': {
                            'driver': 'file',
                            'path': str(self.dataset_path),
                        },
                    }).result()
                    print(f"Successfully opened with {fallback_driver} driver")
                    break
                except Exception as e2:
                    print(f"Failed with {fallback_driver}: {e2}")
                    continue
            else:
                raise RuntimeError(f"Could not open dataset with any supported driver (zarr, zarr3, n5)")
        
        # Get dataset shape and determine image dimensions
        self.shape = self.dataset.shape
        self.ndim = len(self.shape)
        
        # Determine which dimensions are for the stack and which for images
        if self.ndim == 3:
            # Assume first dimension is stack index
            self.n_images = self.shape[0]
            self.image_shape = self.shape[1:]
        elif self.ndim == 4:
            # Could be (stack, height, width, channels) or (stack, channels, height, width)
            self.n_images = self.shape[0]
            self.image_shape = self.shape[1:]
        else:
            raise ValueError(f"Unsupported dataset dimensions: {self.shape}")
        
        print(f"Dataset loaded successfully!")
        print(f"  Path: {self.dataset_path}")
        print(f"  Shape: {self.shape}")
        print(f"  Number of images: {self.n_images}")
        print(f"  Image shape: {self.image_shape}")
        print(f"  Data type: {self.dataset.dtype}")
        
    def _get_image(self, index):
        """Get a single image from the dataset."""
        if index < 0 or index >= self.n_images:
            return None

        # Read the image at the given index
        if self.ndim == 3:
            image = self.dataset[index, :, :].read().result()
        elif self.ndim == 4:
            image = self.dataset[index, ...].read().result()
            # If 4D, might need to handle channel dimension
            if image.shape[-1] in [1, 3, 4]:  # Channels last
                if image.shape[-1] == 1:
                    image = image.squeeze(-1)
            elif image.shape[0] in [1, 3, 4]:  # Channels first
                if image.shape[0] == 1:
                    image = image[0]
                else:
                    image = np.transpose(image, (1, 2, 0))

        # Apply transpose if requested
        if self.transpose and len(image.shape) == 2:
            image = image.T

        return image
    
    def _setup_gui(self):
        """Setup the matplotlib GUI."""
        self.fig, self.ax = plt.subplots(figsize=(10, 8))
        plt.subplots_adjust(bottom=0.25)
        
        # Display first image
        self.current_image = self._get_image(0)
        self.im = self.ax.imshow(self.current_image, cmap='gray' if len(self.current_image.shape) == 2 else None)
        self.ax.set_title(f"Image {self.current_index + 1}/{self.n_images}")
        self.ax.axis('on')
        
        # Add colorbar
        self.colorbar = plt.colorbar(self.im, ax=self.ax)
        
        # Create slider for navigation
        ax_slider = plt.axes([0.2, 0.1, 0.5, 0.03])
        self.slider = Slider(
            ax_slider, 'Image', 
            0, self.n_images - 1, 
            valinit=0, valstep=1,
            valfmt='%d'
        )
        self.slider.on_changed(self._update_image)
        
        # Create previous/next buttons
        ax_prev = plt.axes([0.2, 0.05, 0.1, 0.04])
        self.btn_prev = Button(ax_prev, 'Previous')
        self.btn_prev.on_clicked(self._prev_image)
        
        ax_next = plt.axes([0.31, 0.05, 0.1, 0.04])
        self.btn_next = Button(ax_next, 'Next')
        self.btn_next.on_clicked(self._next_image)
        
        # Create text box for direct navigation
        ax_textbox = plt.axes([0.5, 0.05, 0.1, 0.04])
        self.textbox = TextBox(ax_textbox, 'Go to:', initial=str(self.current_index + 1))
        self.textbox.on_submit(self._goto_image)
        
        # Add statistics button
        ax_stats = plt.axes([0.75, 0.05, 0.1, 0.04])
        self.btn_stats = Button(ax_stats, 'Stats')
        self.btn_stats.on_clicked(self._show_stats)
        
        # Add keyboard shortcuts
        self.fig.canvas.mpl_connect('key_press_event', self._on_key)
        
        # Display info
        self._update_display_info()
        
    def _update_image(self, val):
        """Update displayed image when slider changes."""
        self.current_index = int(self.slider.val)
        self.current_image = self._get_image(self.current_index)
        
        if self.current_image is not None:
            # Update image data
            self.im.set_data(self.current_image)
            
            # Update color limits
            vmin, vmax = self.current_image.min(), self.current_image.max()
            self.im.set_clim(vmin, vmax)
            
            # Update display
            self._update_display_info()
            self.fig.canvas.draw_idle()
    
    def _prev_image(self, event):
        """Go to previous image."""
        if self.current_index > 0:
            self.slider.set_val(self.current_index - 1)
    
    def _next_image(self, event):
        """Go to next image."""
        if self.current_index < self.n_images - 1:
            self.slider.set_val(self.current_index + 1)
    
    def _goto_image(self, text):
        """Go to specific image number."""
        try:
            # Convert from 1-indexed to 0-indexed
            index = int(text) - 1
            if 0 <= index < self.n_images:
                self.slider.set_val(index)
            else:
                print(f"Image number must be between 1 and {self.n_images}")
        except ValueError:
            print(f"Invalid image number: {text}")
    
    def _show_stats(self, event):
        """Display statistics for current image."""
        if self.current_image is not None:
            stats_text = (
                f"Image {self.current_index + 1}/{self.n_images}\n"
                f"Shape: {self.current_image.shape}\n"
                f"Min: {self.current_image.min():.4f}\n"
                f"Max: {self.current_image.max():.4f}\n"
                f"Mean: {self.current_image.mean():.4f}\n"
                f"Std: {self.current_image.std():.4f}"
            )
            print("\n" + "="*30)
            print(stats_text)
            print("="*30)
    
    def _on_key(self, event):
        """Handle keyboard shortcuts."""
        if event.key == 'left':
            self._prev_image(None)
        elif event.key == 'right':
            self._next_image(None)
        elif event.key == 'home':
            self.slider.set_val(0)
        elif event.key == 'end':
            self.slider.set_val(self.n_images - 1)
        elif event.key == 's':
            self._show_stats(None)
        elif event.key == 'h':
            self._show_help()
    
    def _show_help(self):
        """Display help information."""
        help_text = """
Keyboard Shortcuts:
  ← / →     : Previous/Next image
  Home/End  : First/Last image
  s         : Show statistics
  h         : Show this help
  
Mouse:
  Use slider to navigate
  Click buttons for navigation
  Enter image number in text box (1-indexed)
        """
        print(help_text)
    
    def _update_display_info(self):
        """Update the display information."""
        self.ax.set_title(
            f"Image {self.current_index + 1}/{self.n_images} | "
            f"Shape: {self.current_image.shape} | "
            f"Range: [{self.current_image.min():.2f}, {self.current_image.max():.2f}]"
        )
    
    def show(self):
        """Display the viewer."""
        print("\nViewer Controls:")
        print("  Use arrow keys or buttons to navigate")
        print("  Press 'h' for help")
        plt.show()


def main():
    """Main entry point for the script."""
    parser = argparse.ArgumentParser(
        description='View tensorstore datasets containing image stacks',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # View a zarr v2 dataset (auto-detected)
  python tensorstore_viewer.py /path/to/dataset.zarr
  
  # View a zarr v3 dataset
  python tensorstore_viewer.py /path/to/dataset.zarr --driver zarr3
  
  # View an n5 dataset
  python tensorstore_viewer.py /path/to/dataset.n5
  
  # Specify driver explicitly for zarr v2
  python tensorstore_viewer.py /path/to/dataset --driver zarr
        """
    )
    
    parser.add_argument(
        'dataset',
        type=str,
        help='Path to the tensorstore dataset'
    )
    
    parser.add_argument(
        '--driver',
        type=str,
        choices=['zarr', 'zarr3', 'n5', 'neuroglancer_precomputed'],
        default=None,
        help='TensorStore driver to use: zarr (v2), zarr3 (v3), n5, etc. (default: auto-detect)'
    )
    
    parser.add_argument(
        '--spec',
        type=str,
        default=None,
        help='JSON file containing a complete tensorstore spec'
    )

    parser.add_argument(
        '--transpose',
        action='store_true',
        help='Transpose images (swap height and width dimensions)'
    )

    args = parser.parse_args()

    # Build spec if driver is specified
    spec = None
    if args.spec:
        import json
        with open(args.spec, 'r') as f:
            spec = json.load(f)
    elif args.driver:
        spec = {
            'driver': args.driver,
            'kvstore': {
                'driver': 'file',
                'path': args.dataset,
            }
        }

    # Create and show viewer
    try:
        viewer = TensorStoreViewer(args.dataset, spec, transpose=args.transpose)
        viewer.show()
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
