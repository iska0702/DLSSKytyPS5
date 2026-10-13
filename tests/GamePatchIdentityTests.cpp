#include "loader/gamePatchIdentity.h"
#include <cstdio>
#include <cstdlib>

static void Check(bool ok) {
	if (!ok) std::abort();
}

int main(int argc, char** argv) {
	using namespace Loader::GamePatch;
	const auto path = std::filesystem::temp_directory_path() / "kyty-game-patch-identity-test.bin";
	{
		std::ofstream file(path, std::ios::binary);
		file << "abc";
	}
	const std::string hash = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
	Check(PatchExecutableMatches(path, hash));
	Check(PatchExecutableMatches(path, "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"));
	Check(!PatchExecutableMatches(path, std::string(64, '0')));
	Check(!PatchExecutableMatches(path, "*"));
	Check(!IsPatchSHA256(std::string(64, 'z')));
	{
		std::ofstream file(path, std::ios::binary);
	}
	Check(PatchExecutableMatches(path, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
	std::filesystem::remove(path);
	Check(!PatchExecutableMatches(path, hash));
	if (argc == 3) Check(PatchExecutableMatches(argv[1], argv[2]));
	std::puts("Game patch SHA-256 identity tests passed");
}
