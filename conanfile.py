import os

from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout
from conan.tools.files import copy
from conan.tools.apple import is_apple_os


class slangSandBoxRecipe(ConanFile):
    name = "slang-sandbox"
    version = "0.1.0"
    generators = "CMakeDeps", "CMakeToolchain"

    # Optional metadata
    license = "https://github.com/gipeto/slangy/blob/main/LICENSE"
    author = "Javier Perez"
    url = "https://github.com/gipeto/slang-sandbox"
    description = "Experimenting with slang shaders"
    topics = ("slang", "shaders")

    # Binary configuration
    settings = "os", "compiler", "build_type", "arch"

    def requirements(self):
        self.requires("slangy/0.3.0")
        self.requires("slang/2026.3.1")

        if is_apple_os(self):
            self.requires("metal-cpp/26")

    def layout(self):
        cmake_layout(self)
        self.folders.build = "build"

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
