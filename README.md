# SmartGurd Pro

SmartGurd Pro is a cross-platform Flutter application designed for secure mobile and desktop experiences. It includes a modern UI, embedded cellular support, and flexible platform targets for Android, iOS, web, Windows, Linux, and macOS.

## Key Features

- Flutter-based UI with responsive layouts
- Multi-platform support: Android, iOS, web, Windows, macOS, Linux
- Embedded cellular or device connectivity support
- Modular architecture with platform-specific assets in `android/`, `ios/`, `web/`, `linux/`, `macos/`, and `windows/`
- Test scaffolding available under `test/`

## Project Structure

- `lib/` — Flutter application source code
- `android/`, `ios/`, `web/`, `linux/`, `macos/`, `windows/` — platform-specific project files
- `pubspec.yaml` — Flutter package configuration
- `README.md` — project overview and setup details
- `CONTRIBUTING.md` — contribution guidelines

## Getting Started

Install Flutter and ensure the SDK is available in your PATH.

1. Install dependencies:

```bash
flutter pub get
```

2. Run on your target platform:

```bash
flutter run
```

3. Build for a specific platform:

```bash
flutter build apk
flutter build ios
flutter build web
flutter build macos
flutter build windows
flutter build linux
```

## Notes

- Use `flutter doctor` to verify platform-specific toolchain requirements.
- The app is suited for both mobile and desktop deployments.
- Customize UI and logic under `lib/`.

## Contributing

See `CONTRIBUTING.md` for contribution workflow, coding standards, and commit conventions.
