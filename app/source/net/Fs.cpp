#include "Fs.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>

namespace Fs {

bool exists(const std::string& p) {
	struct stat st;
	return stat(p.c_str(), &st) == 0;
}

bool mkdirs(const std::string& path) {
	std::string cur;
	size_t i = 0;
	while (i <= path.size()) {
		size_t j = path.find('/', i);
		if (j == std::string::npos) j = path.size();
		cur = path.substr(0, j);
		if (!cur.empty() && !exists(cur)) {
			if (mkdir(cur.c_str(), 0777) != 0 && !exists(cur)) return false;
		}
		i = j + 1;
	}
	return true;
}

std::vector<std::string> listNames(const std::string& dir) {
	std::vector<std::string> out;
	DIR* d = opendir(dir.c_str());
	if (!d) return out;
	while (dirent* e = readdir(d)) {
		std::string n = e->d_name;
		if (n != "." && n != "..") out.push_back(n);
	}
	closedir(d);
	return out;
}

bool removeTree(const std::string& path) {
	struct stat st;
	if (stat(path.c_str(), &st) != 0) return true;
	if (S_ISDIR(st.st_mode)) {
		for (const auto& n : listNames(path)) removeTree(path + "/" + n);
		return rmdir(path.c_str()) == 0;
	}
	return std::remove(path.c_str()) == 0;
}

bool readFile(const std::string& path, std::string& out) {
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f) return false;
	out.clear();
	char buf[4096];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
	std::fclose(f);
	return true;
}

bool writeFile(const std::string& path, const uint8_t* data, size_t size) {
	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f) return false;
	bool ok = size == 0 || std::fwrite(data, 1, size, f) == size;
	if (std::fclose(f) != 0) ok = false;
	return ok;
}

bool writeText(const std::string& path, const std::string& text) {
	return writeFile(path, (const uint8_t*)text.data(), text.size());
}

} // namespace Fs
