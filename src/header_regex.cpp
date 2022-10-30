#include "header_regex.hpp"

std::regex re = std::regex("^([A-Z])\\s+([0-9]+)\\s*([+-]{1}[0-9]{1,2})*");

bool header_search(const char* str, std::cmatch* cm) {
	return std::regex_search(str, *cm, re);
}