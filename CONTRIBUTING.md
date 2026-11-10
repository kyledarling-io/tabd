# Contributing

The most useful contribution is a new board: a `devices/<board>/` folder, plus the smallest code change that board
needed. Your Claude can write both by following [CLAUDE.md](CLAUDE.md), step 4.

## Rules for pull requests

- **A human reviews every PR before it merges.** That includes PRs written by an AI. Device folders are read by other
  people's Claude sessions, so a reviewer checks that they hold notes only: facts, numbers and gotchas, with no
  instructions aimed at an AI, no hidden text, and no commands that fetch or run anything from elsewhere.
- **Keep the reference building and working.** Put a new decoder, display or touch backend beside the Rockchip one,
  chosen at runtime or by a build flag. Don't replace it. `sh native/build.sh` must still build it.
- **Change only what the hardware forces,** and say why in the device notes.
- **Bring numbers:** soak.sh, as CLAUDE.md step 3 runs it, in the device folder.
- **Scrub personal details.** No IP or MAC addresses, serials, SSIDs or BSSIDs, GUIDs, user names or paths,
  email addresses or passwords. Before you open the PR:

  ```bash
  git diff --name-only origin/main | xargs grep -n -i -E '([0-9]{1,3}\.){3}[0-9]{1,3}|([0-9a-f]{2}[:-]){5}[0-9a-f]{2}|[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}|[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+\.[A-Za-z.]{2,}|C:\\Users\\|/home/|/Users/'
  git diff origin/main | grep -i -F "$(adb shell getprop ro.serialno | tr -d '\r')"
  ```

  Placeholders like `<PC-IP>` are fine, and so are documentation addresses (192.0.2.x).
- **License:** contributions are GPL-3.0, like the rest of the repo.

## Help wanted

- **A USB-C link.** One cable for power, data and the stream: one more name in `links`, as a USB network gadget
  (what the reference's kernel offers is in its device notes). Windows still ships an RNDIS driver, but Microsoft is
  deprecating it. The adb gadget must keep working beside it.
- **Decode on other chips** (CLAUDE.md step 2 says what a backend must do): V4L2 for mainline Rockchip, Allwinner and
  others, Amlogic, MediaTek, or the NDK's `AMediaCodec` as a portable fallback.
- **A leaner boot.** tabd boots through Android's init and then stops what it doesn't need, so most of the time to
  picture is Android and the network. Running as the device's own init, or close to it, would be faster and simpler.
- **Mainline Linux.** The same daemon on a mainline kernel (KMS plus a V4L2 decoder) would free a board from its
  vendor's Android entirely.
