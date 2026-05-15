export interface TestContentItem {
  name: string;
  path: string;
  type: "image" | "sequence" | "overlay";
  description: string;
  category: string;
}

export const testContent: TestContentItem[] = [
  // Images
  {
    name: "Color Test",
    path: "images/color-test.png",
    type: "image",
    description: "RGB color test pattern",
    category: "Test Patterns",
  },
  {
    name: "Frogger",
    path: "images/frogger.png",
    type: "image",
    description: "Frogger sprite",
    category: "Sprites",
  },
  {
    name: "Pac-Man Test",
    path: "images/pacman-test.png",
    type: "image",
    description: "Pac-Man sprite test",
    category: "Sprites",
  },
  {
    name: "Zaxxon",
    path: "images/zaxxon.png",
    type: "image",
    description: "Zaxxon sprite",
    category: "Sprites",
  },
  // Sequences
  {
    name: "Pac-Man Ghost",
    path: "images/pac-ghost",
    type: "sequence",
    description: "Animated ghost sprite",
    category: "Animations",
  },
  {
    name: "Lizard Sprite",
    path: "images/lizard-sprite",
    type: "sequence",
    description: "Lizard animation",
    category: "Animations",
  },
  {
    name: "Sprite Test",
    path: "images/sprite-test",
    type: "sequence",
    description: "Sprite test sequence",
    category: "Animations",
  },
  // Overlays
  {
    name: "Falling Leaves",
    path: "overlays/leaves-falling",
    type: "overlay",
    description: "Falling leaves effect",
    category: "Effects",
  },
  // Fonts
  {
    name: "Font Character Set (3x5)",
    path: "images/font-3x5-sheet.png",
    type: "image",
    description: "3x5 tiny font (used in interface)",
    category: "Test Patterns",
  },
  {
    name: "Font Character Set (5x7)",
    path: "images/font-5x7-sheet.png",
    type: "image",
    description: "5x7 standard font (larger)",
    category: "Test Patterns",
  },
  // System
  {
    name: "Panel Configuration",
    path: "system/idle",
    type: "image",
    description: "Display panel settings and info",
    category: "System",
  },
];
