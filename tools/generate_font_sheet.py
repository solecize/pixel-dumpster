#!/usr/bin/env python3
"""
Generate font sheet PNGs from the bitmap fonts used by the device.
Reads font data from pd-display.cpp and creates visual reference images.
Supports both the 5x7 font and the 3x5 tiny font.
"""

import re
import sys
from pathlib import Path

# Font specs
FONTS = {
    '5x7': {
        'width': 5,
        'height': 7,
        'cell_w': 6,
        'cell_h': 9,
        'array_name': 'pd_font_5x7',
        'output': 'font-5x7-sheet.png',
    },
    '3x5': {
        'width': 3,
        'height': 5,
        'cell_w': 4,
        'cell_h': 6,
        'array_name': 'pd_font_tiny5',
        'output': 'font-3x5-sheet.png',
    }
}

def parse_font_data(cpp_file, font_spec):
    """Extract font data from pd-display.cpp"""
    content = cpp_file.read_text()
    array_name = font_spec['array_name']
    height = font_spec['height']
    
    # Find the font array - match array name and height variable
    pattern = rf'static const uint8_t {array_name}\[96\]\[.*?\] = \{{(.*?)\}};'
    match = re.search(pattern, content, re.DOTALL)
    if not match:
        raise ValueError(f"Could not find {array_name} in pd-display.cpp")
    
    font_data = match.group(1)
    
    # Parse each character (96 chars total, ASCII 32-127)
    chars = []
    # Find all character definitions - number of bytes = height
    hex_pattern = '0x[0-9A-Fa-f]{1,2}'
    char_pattern = rf'\{{({hex_pattern}(?:,{hex_pattern}){{{height-1}}})\}}'
    
    for match in re.finditer(char_pattern, font_data):
        bytes_str = match.group(1)
        rows = [int(b, 16) for b in bytes_str.split(',')]
        if len(rows) != height:
            continue
        chars.append(rows)
    
    if len(chars) != 96:
        raise ValueError(f"Expected 96 characters for {array_name}, found {len(chars)}")
    
    return chars

def create_font_sheet(font_data, font_spec, output_path):
    """Create a PNG showing all 96 characters in a grid"""
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print("ERROR: Pillow is not installed. Install with: pip3 install Pillow")
        return False
    
    # Layout: 8 columns x 12 rows
    cols = 8
    rows = 12
    
    font_w = font_spec['width']
    font_h = font_spec['height']
    cell_w = font_spec['cell_w']
    cell_h = font_spec['cell_h']
    
    # Image size
    img_width = cols * cell_w
    img_height = rows * cell_h
    
    # Create black image
    img = Image.new('RGB', (img_width, img_height), (0, 0, 0))
    pixels = img.load()
    
    # Draw each character
    for idx, char_rows in enumerate(font_data):
        col = idx % cols
        row = idx // cols
        
        # Base position for this character cell
        x_base = col * cell_w
        y_base = row * cell_h
        
        # Draw the glyph
        for y in range(font_h):
            row_bits = char_rows[y]
            for x in range(font_w):
                # Bit (width-1) is leftmost pixel
                if row_bits & (1 << (font_w - 1 - x)):
                    pixels[x_base + x, y_base + y] = (255, 255, 255)
    
    img.save(output_path)
    print(f"✓ Font sheet generated: {output_path}")
    print(f"  Size: {img_width}x{img_height} ({cols}x{rows} grid, {font_w}x{font_h} chars)")
    print(f"  Characters: 96 (ASCII 32-127)")
    return True

def main():
    # Find the source file
    script_dir = Path(__file__).parent
    project_root = script_dir.parent
    cpp_file = project_root / "components/pd-display/pd-display.cpp"
    
    if not cpp_file.exists():
        print(f"ERROR: Could not find {cpp_file}")
        return 1
    
    print(f"Reading font data from: {cpp_file}\n")
    
    # Generate both fonts
    for font_name, font_spec in FONTS.items():
        output_file = project_root / "content/images" / font_spec['output']
        
        print(f"Generating {font_name} font sheet...")
        try:
            font_data = parse_font_data(cpp_file, font_spec)
            print(f"  ✓ Parsed {len(font_data)} characters")
            
            if not create_font_sheet(font_data, font_spec, output_file):
                return 1
        except Exception as e:
            print(f"  ERROR: {e}")
            return 1
        print()
    
    return 0

if __name__ == "__main__":
    exit(main())
