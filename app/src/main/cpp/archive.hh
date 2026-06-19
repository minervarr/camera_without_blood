#pragma once

#include <string>

struct android_app;

// Restored from the old archive_engine (commit 537b933 of that submodule); the
// library was since rewritten with an incompatible API and this path helper is
// the only piece the app used.
namespace archive {

// Resolves the absolute path to the public Documents directory with an optional subfolder.
// Creates the directory structure if it doesn't exist.
std::string get_documents_path(android_app* app, const std::string& subfolder = "");

} // namespace archive
