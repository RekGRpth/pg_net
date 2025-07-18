{ lib, fetchFromGitHub, nginx, nginxModules, writeShellScriptBin } :

let
  ngx_pathological = rec {
    name = "ngx_pathological";
    version = "0.1";
    src = fetchFromGitHub {
      owner  = "steve-chavez";
      repo   = name;
      rev    = "668126a815daaf741433409a5afff5932e2fb2af";
      sha256 = "sha256-tl7NoPlQCN9DDYQLRrHA3bP5csqbXUW9ozLKPbH2dfI=";
    };
    meta = with lib; {
      license = with licenses; [ mit ];
    };
  };
  customNginx = nginx.override {
    configureFlags = ["--with-cc='c99'"];
    modules = [
      nginxModules.echo
      ngx_pathological
    ];
  };
  script = ''
    set -euo pipefail

    export PATH=${customNginx}/bin:"$PATH"

    nginx -p nix/nginx -e stderr &

    pidfile=nix/nginx/nginx.pid
    while [ ! -s "$pidfile" ]; do sleep 0.05; done
    pid=$(cat "$pidfile")

    trap 'kill -TERM "$pid" 2>/dev/null || true' sigint sigterm exit

    "$@"
  '';
in
{
  customNginx = customNginx;
  nginxScript = writeShellScriptBin "net-with-nginx" script;
}
