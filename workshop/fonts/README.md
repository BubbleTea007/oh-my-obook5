# Fonts

The device and simulator use **LXGW WenKai** (霞鹜文楷), an open-source CJK
font under the SIL Open Font License 1.1:

- upstream: https://github.com/lxgw/LxgwWenKai
- license: https://github.com/lxgw/LxgwWenKai/blob/main/OFL.txt

It is included as a Git submodule at:

```text
workshop/fonts/LxgwWenKai
```

Initialize it with:

```bash
git submodule update --init
```

Fonts used:

```text
workshop/fonts/LxgwWenKai/fonts/TTF/LXGWWenKai-Regular.ttf
workshop/fonts/LxgwWenKai/fonts/TTF/LXGWWenKai-Medium.ttf
```

The packaging script copies these two files to the device as:

```text
/user/workshop/fonts/LXGWWenKai-Regular.ttf
/user/workshop/fonts/LXGWWenKai-Medium.ttf
```

No TTF files are stored directly in this repository.
