{
  description = "Waywall fork with Twitch chat overlay support";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
    in
    {
      packages.${system}.default = pkgs.stdenv.mkDerivation {
        pname = "waywall-chat";
        version = "0-unstable";

        src = ./.;

        nativeBuildInputs = with pkgs; [
          meson
          ninja
          pkg-config
          wayland-scanner
        ];

        buildInputs = with pkgs; [
          libGL
          libspng
          libxkbcommon
          luajit
          wayland
          wayland-protocols
          wayland-scanner
          libxkbcommon
          # EGL/GL
          libGL
          mesa
          libgbm
          # XCB dependencies
          xorg.libxcb
          xorg.xcbutilwm
          xorg.xcbutilerrors
          xwayland
          # Additional dependencies for this fork
          freetype
          libavif
          curl
          libircclient
        ];

        shellHook = ''
          export PKG_CONFIG_PATH=${pkgs.libgbm}/lib/pkgconfig:$PKG_CONFIG_PATH
          export INTEL_DEBUG=noccs
        '';

        installPhase = ''
          runHook preInstall
          install -Dm755 waywall/waywall -t $out/bin
          runHook postInstall
        '';

        meta = {
          description = "Waywall fork with Twitch chat overlay";
          license = pkgs.lib.licenses.gpl3Only;
          platforms = pkgs.lib.platforms.linux;
          mainProgram = "waywall";
        };
      };

      devShells.${system}.default = pkgs.mkShell {
        inputsFrom = [ self.packages.${system}.default ];
      };
    };
}