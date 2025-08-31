# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

BOIII is a Call of Duty: Black Ops 3 client modification developed in C++20. This is a fork of the original BOIII client, providing enhanced functionality and bug fixes for the game. The project uses a component-based architecture with Steam API integration and custom networking.

## Build System & Commands

### Build Generation
```bash
# Generate Visual Studio 2022 project files
generate.bat

# Alternative: use premake5 directly
tools/premake5.exe vs2022

# Generate with custom options
tools/premake5.exe --dev-build vs2022           # Enable development builds
tools/premake5.exe --no-check vs2022            # Disable ownership checks
tools/premake5.exe --copy-to="C:\path" vs2022   # Copy binary after build
```

### Build Process
- After running `generate.bat`, open `build/boiii.sln` in Visual Studio
- Build configurations: Debug, Release
- Target platform: x64 only
- Main executable: `boiii.exe` (renamed from "client" project)

### Version Management
```bash
# Get current version string
tools/premake5.exe version

# Generate build info headers (run automatically during build)
tools/premake5.exe generate-buildinfo
```

## Code Architecture

### Component System
The project uses a component-based architecture centered around the `component_loader`:

- **Registration**: Components register themselves using `REGISTER_COMPONENT(name)` macro
- **Lifecycle**: Components follow `activate()` → `post_load()` → `post_unpack()` → `pre_destroy()` lifecycle
- **Types**: Components specify their type (client/server/both) via `component_type`
- **Location**: All components in `src/client/component/`

### Core Structure
```
src/
├── client/           # Main client code
│   ├── component/    # Modular components (auth, network, etc.)
│   ├── game/         # Game integration layer
│   │   ├── demonware/    # Network service emulation
│   │   └── ui_scripting/ # Lua scripting support
│   ├── launcher/     # HTML-based launcher UI
│   ├── loader/       # Binary loading and component management
│   ├── steam/        # Steam API integration
│   └── updater/      # Auto-update functionality
├── common/           # Shared utilities
│   ├── exception/    # Crash handling
│   └── utils/        # Core utilities (hooks, memory, etc.)
└── tlsdll/          # Thread-local storage DLL
```

### Key Systems

**Binary Loading**: The client loads either `BlackOps3.exe` (client) or `BlackOps3_UnrankedDedicatedServer.exe` (server) into memory and hooks the Steam API.

**Steam Integration**: Patches Steam API calls to provide offline functionality and custom networking.

**Demonware Emulation**: Reimplements Activision's backend services locally for offline play.

**Component Examples**:
- `auth`: Handles authentication and user management
- `network`: Custom networking layer
- `party`: Multiplayer party system
- `console`: In-game console functionality
- `updater`: Auto-update mechanism

## Development Guidelines

### Adding New Components
1. Create component class inheriting from `generic_component`
2. Implement required methods (`activate()`, `post_load()`, etc.)
3. Use `REGISTER_COMPONENT(your_component)` at the end of the file
4. Place in `src/client/component/`

### Memory Management
- Use RAII patterns throughout
- Utilize `utils::memory` for game memory operations
- Use `utils::hook` for runtime patching

### Game Integration
- Game structures and functions are in `src/client/game/`
- Use `game::` namespace functions for game-specific operations
- Steam API hooks should go through the `steam::` namespace

### Networking
- Custom protocol implementations go in `src/client/game/demonware/`
- Follow existing service patterns for new network services

## File Locations

- **Build output**: `build/bin/x64/[Debug|Release]/`
- **Generated files**: `src/version.h`, `src/version.hpp` (auto-generated)
- **Game data**: `data/` directory (scripts, UI, configs)
- **Dependencies**: `deps/` directory with premake build files

## Important Notes

- The project requires Visual Studio 2022 with C++20 support
- Must be placed in Call of Duty: Black Ops 3 installation directory
- Uses premake5 as build system generator (not CMake)
- Targets Windows x64 only
- Steam must be installed for the game to run