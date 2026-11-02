# Fajk Studio

An experimental image and video codec with its own desktop encoder and player.

## What is it for?

Fajk Studio is an experimental image and video codec with a Windows encoder, player, command-line tool and decoder SDK. It is for exploring compact image and video storage with control over the size, speed and reconstruction of the result.

## Features

- Set a target size and choose an encoding mode that balances speed and quality.
- Process and preview images and video using CPU or supported GPU paths.
- Choose independent frames or optional inter-frame compression for video.
- Adjust image reconstruction and final sharpening, and inspect the result before export.
- Use the included CLI or decoder SDK to integrate supported files into other software.

## Download and use

1. Download the R600.4 Windows ZIP from [Releases](https://github.com/FajkPes/Fajk-Studio/releases/latest).
2. Extract the entire archive and run `Spustit Fajk Studio.cmd`.
3. Open an image or video, choose a size and encoding mode, then preview and export.

**Fast** favors speed, **More quality** spends longer on the search, and **Max compression · Fast** uses the Fast search with additional compression choices. Results depend on the source and settings; a target does not imply a fixed visual quality. The format is experimental and requires a compatible Fajk decoder. R600.4 output needs the corresponding decoder for its newer model.

For technical details, measurements and limitations, see [dev/docs/CURRENT.md](dev/docs/CURRENT.md).

[Detailed documentation](README-DETAILS.md) · [All releases](https://github.com/FajkPes/Fajk-Studio/releases)

## Choosing a video method

The encoder can store frames independently or use inter-frame compression. **Coefficients** recodes independently encoded frame coefficients without changing their decoded pixels. **Regions** reuses similar areas at the same position with a configurable tolerance. **Max compression** is an experimental motion-prediction method: it can save more space, but higher strength can remove detail. These choices are separate from the Fast / More quality encoding-speed modes.

## Credits

**Idea, concept and design:** FajkPes  
**Coding:** ChatGPT Codex, Claude and Google Antigravity
