#include "cli/cli.hpp"
int main(int argc, char *argv[]) {
  mokai::Cli app;
  return app.Run(argc, argv);
}
