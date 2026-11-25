# Frontend

The React/Vite frontend contains the custom MPEG-DASH player (App.jsx), the competing dash.js component, and the experiment results display.

From this directory, install the dependencies listed in the supplied lockfile with `npm ci`. The competing player also imports `dashjs`, which is absent from the supplied manifest and lockfile; install the appropriate dash.js version locally (e.g. `npm install --no-save --package-lock=false dashjs`). The exact version used in the original experiments was not captured in these files. The frontend development command is `npm run dev`.

The player requires the C++ backend, compatible DASH media assets and a prepared Linux/WSL experiment environment. See the [root README](../README.md) for the architecture and limitations. A clean build and end-to-end reproduction have not been independently verified for this repository snapshot.
