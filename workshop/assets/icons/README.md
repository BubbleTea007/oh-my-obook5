# Icon sources

The launcher's built-in UI icons are rasterized from open-source vector icons:

- **Lucide** — ISC license, <https://lucide.dev/>
  - `wifi-arc3`, `wifi-arc2`, `wifi-arc1`, `wifi-dot` — WiFi signal layers
  - `battery-full`, `battery-medium`, `battery-low`, `battery-empty`,
    `battery-charging` — battery status
  - `layout-dashboard` — default App icon

They are rendered to A8 alpha masks by `tools/gen_icons.py`; do not hand-edit
`src/icons.c`.

App icons are not generated here. Each App provides `app/<id>/icon.png` and
the launcher loads it directly at runtime. License texts are in
`assets/icons/license/`.
