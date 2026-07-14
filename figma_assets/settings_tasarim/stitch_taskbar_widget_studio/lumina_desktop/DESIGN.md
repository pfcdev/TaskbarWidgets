---
name: Lumina Desktop
colors:
  surface: '#111318'
  surface-dim: '#111318'
  surface-bright: '#37393f'
  surface-container-lowest: '#0c0e13'
  surface-container-low: '#1a1b21'
  surface-container: '#1e1f25'
  surface-container-high: '#282a2f'
  surface-container-highest: '#33353a'
  on-surface: '#e2e2e9'
  on-surface-variant: '#c9c4d8'
  inverse-surface: '#e2e2e9'
  inverse-on-surface: '#2e3036'
  outline: '#938ea1'
  outline-variant: '#484555'
  surface-tint: '#cabeff'
  primary: '#cabeff'
  on-primary: '#32009a'
  primary-container: '#947dff'
  on-primary-container: '#2b0088'
  inverse-primary: '#613de0'
  secondary: '#5fd4ff'
  on-secondary: '#003545'
  secondary-container: '#00b1de'
  on-secondary-container: '#003f51'
  tertiary: '#c3c6d3'
  on-tertiary: '#2c303a'
  tertiary-container: '#8d909c'
  on-tertiary-container: '#262a33'
  error: '#ffb4ab'
  on-error: '#690005'
  error-container: '#93000a'
  on-error-container: '#ffdad6'
  primary-fixed: '#e6deff'
  primary-fixed-dim: '#cabeff'
  on-primary-fixed: '#1c0062'
  on-primary-fixed-variant: '#4918c8'
  secondary-fixed: '#bbe9ff'
  secondary-fixed-dim: '#5fd4ff'
  on-secondary-fixed: '#001f29'
  on-secondary-fixed-variant: '#004d63'
  tertiary-fixed: '#dfe2ef'
  tertiary-fixed-dim: '#c3c6d3'
  on-tertiary-fixed: '#181c25'
  on-tertiary-fixed-variant: '#434751'
  background: '#111318'
  on-background: '#e2e2e9'
  surface-variant: '#33353a'
typography:
  display:
    fontFamily: Inter
    fontSize: 40px
    fontWeight: '700'
    lineHeight: 52px
    letterSpacing: -0.02em
  headline-lg:
    fontFamily: Inter
    fontSize: 28px
    fontWeight: '600'
    lineHeight: 36px
    letterSpacing: -0.01em
  headline-md:
    fontFamily: Inter
    fontSize: 20px
    fontWeight: '600'
    lineHeight: 28px
  body-lg:
    fontFamily: Inter
    fontSize: 16px
    fontWeight: '400'
    lineHeight: 24px
  body-md:
    fontFamily: Inter
    fontSize: 14px
    fontWeight: '400'
    lineHeight: 20px
  label-md:
    fontFamily: Inter
    fontSize: 12px
    fontWeight: '500'
    lineHeight: 16px
    letterSpacing: 0.02em
  label-sm:
    fontFamily: Inter
    fontSize: 11px
    fontWeight: '600'
    lineHeight: 14px
rounded:
  sm: 0.25rem
  DEFAULT: 0.5rem
  md: 0.75rem
  lg: 1rem
  xl: 1.5rem
  full: 9999px
spacing:
  base: 4px
  xs: 8px
  sm: 12px
  md: 16px
  lg: 24px
  xl: 32px
  sidebar_width: 260px
  container_max_width: 1440px
  gutter: 20px
---

## Brand & Style

This design system is engineered for a premium Windows 11 desktop experience, emphasizing depth, precision, and a sophisticated "Dark Mode" aesthetic. The personality is professional yet futuristic, catering to power users who value both performance and visual elegance.

The visual style is a refined evolution of **Fluent Design**, specifically focusing on **Glassmorphism** and **Minimalism**. It utilizes a deep charcoal foundation to reduce eye strain, while employing "Mica" and "Acrylic" surface behaviors to provide a sense of place and hierarchy. The emotional response should be one of focused immersion, where the UI feels like a high-end physical instrument crafted from dark glass and light.

## Colors

The palette is anchored in a near-black spectrum to provide maximum contrast for the vibrant accent gradients.

- **Foundations:** The core application background uses #0B0D12. Sidebars and navigation areas are slightly lighter (#0D1016) to create structural separation without harsh lines.
- **Accents:** The primary interactive language uses a linear gradient from **Electric Violet (#7C5CFC)** to **Sky Cyan (#39C6F4)**. This gradient should be used sparingly for primary actions, progress indicators, and active states.
- **Semantic Colors:** Success and Error states are high-chroma but balanced to remain legible against dark backgrounds.
- **Borders:** All component borders should use a low-contrast white at 8%–12% opacity to simulate a "beveled glass" edge rather than a solid stroke.

## Typography

The typography system uses **Inter** for its exceptional clarity on high-DPI displays and its systematic, neutral character.

- **Hierarchy:** Use `Display` and `Headline-lg` for dashboard titles. `Body-md` is the standard for all content and input text.
- **Labels:** `Label-sm` should be used for category headers in the sidebar and small metadata, utilizing an uppercase style with slight letter spacing to ensure distinctiveness at small sizes.
- **Rendering:** On Windows platforms, ensure `-webkit-font-smoothing: antialiased` or equivalent is enabled to maintain the crispness of the Inter typeface against the dark background.

## Layout & Spacing

This design system follows a **Fixed Grid** philosophy for desktop optimization.

- **Sidebar:** A fixed 260px vertical navigation occupies the left side. It uses a "Mica" effect (blurred background) to differentiate it from the main content.
- **Content Area:** Content is housed in a flexible container with a maximum width of 1440px. On ultra-wide monitors, this container centers itself with generous outer margins.
- **Rhythm:** An 8px base grid drives all spacing. Standard component gaps are 16px (`md`), while logical sections are separated by 32px (`xl`).
- **Responsive Reflow:** On smaller window sizes (below 1024px), the sidebar collapses into an icon-only rail (72px), and page margins reduce from 32px to 16px.

## Elevation & Depth

Hierarchy is established through **Tonal Layers** and **Glassmorphism** rather than traditional heavy shadows.

- **Layer 0 (Background):** #0B0D12. The canvas for the application.
- **Layer 1 (Sidebar/Secondary):** #0D1016. Used for persistent navigation.
- **Layer 2 (Cards/Surfaces):** #171C26. These represent the primary interactive modules.
- **The Glass Effect:** For modals or floating menus, use a background blur (Backdrop Filter: 20px-30px) with a semi-transparent fill of the primary surface color.
- **Outlines:** Every elevated surface (Layer 1+) must have a 1px solid border. The color is `white` at 10% opacity for the top and side edges, and 5% opacity for the bottom edge, creating a subtle "inner light" effect typical of premium desktop OS environments.

## Shapes

The shape language is consistently rounded to evoke a modern, approachable, and hardware-integrated feel.

- **Main Cards & Containers:** Use `rounded-xl` (24px) for large layout containers and main app cards.
- **Interactive Elements:** Buttons, inputs, and standard components use `rounded-lg` (16px).
- **Pill Elements:** Filter chips, tags, and the toggle switch tracks use a fully rounded (pill) radius to distinguish them from structural containers.

## Components

- **Buttons:** Primary buttons use the Violet-to-Cyan gradient with white text. Secondary buttons are #171C26 with a 1px border.
- **Vertical Navigation:** Active states in the sidebar are indicated by a 4px tall vertical "pill" indicator in the primary accent color placed at the far left of the item.
- **Pill Filters:** Used for view switching. The active filter should have a glass-morphic background (white at 10% opacity) and a subtle 1px border.
- **Windows-Style Toggles:** The toggle track is a dark pill shape; the thumb is a smaller white circle. When "On," the track fills with the primary gradient.
- **Input Fields:** Fields are #151922 with a bottom-only 2px accent border that appears only on focus.
- **Glass-morphic Cards:** Cards should feature a subtle "inner glow" via a 1px border. For featured content, apply a very faint (3% opacity) radial gradient of the primary accent color in the top-right corner of the card.
