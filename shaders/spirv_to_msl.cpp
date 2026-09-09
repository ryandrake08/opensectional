#include <spirv_cross/spirv_msl.hpp>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
    uint32_t decoration(const spirv_cross::Compiler& compiler, const spirv_cross::Resource& resource,
                        spv::Decoration kind)
    {
        if(!compiler.has_decoration(resource.id, kind))
        {
            throw std::runtime_error("SPIR-V resource is missing a descriptor decoration");
        }
        return compiler.get_decoration(resource.id, kind);
    }

    void sort_by_binding(std::vector<spirv_cross::Resource>& resources, const spirv_cross::Compiler& compiler)
    {
        std::sort(resources.begin(), resources.end(), [&compiler](const auto& a, const auto& b)
                  {
                      auto a_set = decoration(compiler, a, spv::DecorationDescriptorSet);
                      auto b_set = decoration(compiler, b, spv::DecorationDescriptorSet);
                      if(a_set != b_set)
                      {
                          return a_set < b_set;
                      }
                      return decoration(compiler, a, spv::DecorationBinding) <
                             decoration(compiler, b, spv::DecorationBinding);
                  });
    }

    void map_buffers(spirv_cross::CompilerMSL& compiler, std::vector<spirv_cross::Resource> resources,
                     uint32_t first_msl_buffer)
    {
        sort_by_binding(resources, compiler);
        for(uint32_t index = 0; index < resources.size(); ++index)
        {
            const auto& resource = resources[index];
            auto binding = spirv_cross::MSLResourceBinding{};
            binding.stage = compiler.get_execution_model();
            binding.basetype = compiler.get_type(resource.type_id).basetype;
            binding.desc_set = decoration(compiler, resource, spv::DecorationDescriptorSet);
            binding.binding = decoration(compiler, resource, spv::DecorationBinding);
            binding.count = 1;
            binding.msl_buffer = first_msl_buffer + index;
            compiler.add_msl_resource_binding(binding);
        }
    }

    std::vector<uint32_t> read_spirv(const std::string& path)
    {
        auto input = std::ifstream(path, std::ios::binary | std::ios::ate);
        if(!input)
        {
            throw std::runtime_error("cannot read " + path);
        }

        auto byte_count = input.tellg();
        if(byte_count <= 0 || byte_count % sizeof(uint32_t) != 0)
        {
            throw std::runtime_error(path + " is not a valid SPIR-V binary");
        }

        std::vector<uint32_t> words(static_cast<size_t>(byte_count) / sizeof(uint32_t));
        input.seekg(0);
        input.read(reinterpret_cast<char*>(words.data()), byte_count);
        if(!input)
        {
            throw std::runtime_error("cannot read " + path);
        }
        return words;
    }
}

int main(int argc, char** argv)
{
    if(argc != 3)
    {
        std::cerr << "usage: spirv_to_msl <input.spv> <output.metal>\n";
        return 1;
    }

    try
    {
        auto compiler = spirv_cross::CompilerMSL(read_spirv(argv[1]));
        auto resources = compiler.get_shader_resources();

        auto uniform_buffers =
            std::vector<spirv_cross::Resource>(resources.uniform_buffers.begin(), resources.uniform_buffers.end());
        auto storage_buffers =
            std::vector<spirv_cross::Resource>(resources.storage_buffers.begin(), resources.storage_buffers.end());
        map_buffers(compiler, std::move(uniform_buffers), 0);
        map_buffers(compiler, std::move(storage_buffers), static_cast<uint32_t>(resources.uniform_buffers.size()));

        auto output = std::ofstream(argv[2]);
        if(!output)
        {
            throw std::runtime_error("cannot write " + std::string(argv[2]));
        }
        output << compiler.compile();
        if(!output)
        {
            throw std::runtime_error("cannot write " + std::string(argv[2]));
        }
    }
    catch(const std::exception& error)
    {
        std::cerr << "spirv_to_msl: " << error.what() << '\n';
        return 1;
    }
}
