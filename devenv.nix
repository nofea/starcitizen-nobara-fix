{ pkgs, ... }:
{
  packages = [ pkgs.vulkan-headers pkgs.vulkan-tools pkgs.gcc pkgs.gdb pkgs.strace pkgs.pkg-config ];
}
