# HP LaserJet Professional M1136 MFP Driver for macOS

A native CUPS print driver for the **HP LaserJet Professional M1136 MFP** on macOS.

*(Printing only; scanner functions are not implemented).*

## Installation

Connect the printer via USB and run:

```bash
sudo ./install.sh
```

The printer will appear in the system print dialog across all applications.

## How it works

The HP LaserJet M1136 is a host-based (GDI) printer using the XQX stream protocol (JBIG-compressed raster wrapped in PJL). macOS's default `rastertohp` filter sends PCL commands, which the printer ignores.

This driver provides a native C CUPS filter (`rastertoxqx`) that:
1. Converts `application/vnd.cups-raster` (from `cgpdftoraster`) directly into the printer's native XQX format.
2. Reads the bidirectional USB backchannel in real-time to report hardware state to macOS:
   - **E0**: Paper Jam
   - **E1**: Out of Paper / Load Paper
   - **E2**: Cover / Door Open
   - **E3**: Cartridge Missing / Unseated

## Credits

- [JBIG-KIT](https://www.cl.cam.ac.uk/~mgk25/jbigkit/) - Markus Kuhn's JBIG lossless image compression library.
- [foo2zjs](https://github.com/koenkooi/foo2zjs) - Rick Richardson's XQX protocol implementation and stream definitions.
