{
  description = "Yolo Board Module for Logos App";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/e9f00bd893984bc8ce46c895c3bf7cac95331127";
    logos-cpp-sdk = {
      url = "github:logos-co/logos-cpp-sdk";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    logos-liblogos = {
      url = "github:logos-co/logos-liblogos";
      inputs.nixpkgs.follows = "nixpkgs";
      inputs.logos-cpp-sdk.follows = "logos-cpp-sdk";
    };
    logos-storage-module = {
      url = "github:logos-co/logos-storage-module";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    logos-delivery-module = {
      url = "github:logos-co/logos-delivery-module";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    logos-package = {
      url = "github:logos-co/logos-package/9e3730d5c0e3ec955761c05b50e3a6047ee4030b";
    };
  };

  outputs = { self, nixpkgs, logos-cpp-sdk, logos-liblogos, logos-storage-module, logos-delivery-module, logos-package, ... }:
    let
      systems = [ "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        pkgs = import nixpkgs { inherit system; };
        logosSdk = logos-cpp-sdk.packages.${system}.default;
        logosLiblogos = logos-liblogos.packages.${system}.default;
        logosStorage = logos-storage-module.packages.${system}.default;
        logosDelivery = logos-delivery-module.packages.${system}.default;
        lgxTool = logos-package.packages.${system}.lgx;
      });
    in
    {
      packages = forAllSystems ({ pkgs, logosSdk, logosLiblogos, logosStorage, logosDelivery, lgxTool }:
        let
          buildInputs = [ pkgs.qt6.qtbase ];

          plugin = pkgs.stdenv.mkDerivation {
            pname = "logos-yolo-board-module";
            version = "0.1.0";
            src = ./.;

            nativeBuildInputs = [
              pkgs.cmake
              pkgs.ninja
              pkgs.pkg-config
              pkgs.patchelf
            ];

            inherit buildInputs;

            # Note: LOGOS_DELIVERY_ROOT intentionally omitted — the module is
            # consumed via raw QRO IPC, no typed wrapper compile-time includes
            # needed. The flake input stays so `nix build` can resolve/pin
            # delivery_module alongside storage for reproducibility.
            cmakeFlags = [
              "-DLOGOS_CPP_SDK_ROOT=${logosSdk}"
              "-DLOGOS_STORAGE_ROOT=${logosStorage}"
              "-GNinja"
            ];

            buildPhase = ''
              runHook preBuild
              ninja yolo_board_module -j''${NIX_BUILD_CORES:-1}
              runHook postBuild
            '';

            installPhase = ''
              runHook preInstall
              mkdir -p $out/lib
              cp libyolo_board_module.so $out/lib/
              runHook postInstall
            '';

            postFixup = ''
              patchelf --set-rpath "$out/lib:${logosLiblogos}/lib:${pkgs.lib.makeLibraryPath buildInputs}" \
                $out/lib/libyolo_board_module.so
            '';

            dontWrapQtApps = true;
          };

          patchManifest = name: metadataFile: ''
            python3 - ${name}.lgx ${metadataFile} <<'PY'
            import json, sys, tarfile, io

            lgx_path = sys.argv[1]
            with open(sys.argv[2]) as f:
                metadata = json.load(f)

            built_variants = {'linux-x86_64-dev', 'linux-amd64-dev'}

            with tarfile.open(lgx_path, 'r:gz') as tar:
                members = [(m, tar.extractfile(m).read() if m.isfile() else None) for m in tar.getmembers()]

            patched = []
            for member, data in members:
                if member.name == 'manifest.json':
                    manifest = json.loads(data)
                    for key in ('name', 'version', 'description', 'type', 'category', 'dependencies'):
                        if key in metadata:
                            manifest[key] = metadata[key]
                    if 'main' in manifest and isinstance(manifest['main'], dict):
                        manifest["main"] = {k.replace("-dev", ""): v for k, v in manifest["main"].items() if k in built_variants}
                    data = json.dumps(manifest, indent=2).encode()
                    member.size = len(data)
                patched.append((member, data))

            with tarfile.open(lgx_path, 'w:gz', format=tarfile.GNU_FORMAT) as tar:
                for member, data in patched:
                    if data is not None:
                        tar.addfile(member, io.BytesIO(data))
                    else:
                        tar.addfile(member)
            PY
          '';

          lgx = pkgs.runCommand "yolo-board-module.lgx" {
            nativeBuildInputs = [ lgxTool pkgs.python3 ];
          } ''
            lgx create yolo-board-module

            mkdir -p variant-files
            cp ${plugin}/lib/libyolo_board_module.so variant-files/

            lgx add yolo-board-module.lgx --variant linux-x86_64-dev --files ./variant-files --main libyolo_board_module.so -y
            lgx add yolo-board-module.lgx --variant linux-amd64-dev --files ./variant-files --main libyolo_board_module.so -y

            lgx verify yolo-board-module.lgx

            ${patchManifest "yolo-board-module" "${self}/metadata.json"}

            mkdir -p $out
            cp yolo-board-module.lgx $out/yolo-board-module.lgx
          '';

        in {
          inherit plugin lgx;
          default = lgx;
        }
      );
    };
}
