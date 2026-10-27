# Contributing to Window Ring

Thanks for helping. Bug reports, ideas and pull requests are all welcome.

## Reporting a bug

[Open an issue](https://github.com/DavidHiFi/window-ring/issues/new) and include:

- what you did, what you expected and what happened instead
- your Windows version and the Window Ring version
- a screenshot of the window with the wrong ring, or no ring, and the app it belongs to
- your `window-ring.ini` if you changed it

## Suggesting a feature

Open an issue that describes what you want to do and why. A screenshot or a mock-up helps.

## Sending a pull request

1. Fork the repo and create a branch from `main`.
2. Build with `./build.ps1` (see the README for the compiler it needs).
3. Run `bin\window-ring.exe`, check the rings on a few normal windows and a few custom-drawn ones, then quit it with `bin\window-ring.exe --stop`.
4. Keep each pull request to one change, and say in the description how you tested it.

All of the code is in `src\window-ring.cpp`. Keep it to system DLLs only, and never load code into other processes.

## License

By contributing, you agree that your work is released under this repo's [MIT License](LICENSE).
