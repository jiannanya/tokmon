#include <snow/c_api.h>

int main() {
  return snow_abi_version_v1() == SNOW_C_ABI_VERSION_V1 ? 0 : 1;
}
