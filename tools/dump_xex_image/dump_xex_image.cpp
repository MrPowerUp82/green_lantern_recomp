// Descompacta o default.xex (decifra e descomprime via XenonUtils) e grava a imagem em memoria do
// guest (layout por RVA, base 0x82000000) num arquivo, para o runtime carregar sem Python.
// Tambem grava <saida>.imports.txt com os imports de VARIAVEIS do kernel (o loader do XEX grava neles
// o endereco da variavel; o runtime faz o mesmo) e <saida>.xex.txt com campos do cabecalho.
//
// Uso: dump_xex_image <default.xex> <saida.image>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#include <image.h>
#include <xex.h>

extern std::unordered_map<size_t, const char*> XamExports;
extern std::unordered_map<size_t, const char*> XboxKernelExports;

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "uso: dump_xex_image <default.xex> <saida.image>\n");
        return 1;
    }

    std::ifstream in(argv[1], std::ios::binary);
    if (!in)
    {
        std::fprintf(stderr, "nao foi possivel abrir %s\n", argv[1]);
        return 1;
    }
    std::vector<uint8_t> xex((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    Image image = Image::ParseImage(xex.data(), xex.size());
    if (!image.data || image.size == 0)
    {
        std::fprintf(stderr, "falha ao interpretar o XEX\n");
        return 1;
    }

    // Registros "tipo 0" da IAT (0x8200xxxx): o loader grava neles o endereco do thunk da funcao
    // (para chamadas indiretas) ou o endereco da variavel exportada pelo kernel.
    // Saida: "func <registro> <thunk> <nome>" ou "var <registro> <nome>".
    std::string importsPath = std::string(argv[2]) + ".imports.txt";
    std::ofstream imports(importsPath);
    std::unordered_map<std::string, size_t> thunkByName;
    std::unordered_set<size_t> thunkAddresses;
    for (const auto& symbol : image.symbols)
    {
        if (symbol.type == Symbol_Function && symbol.name.rfind("__imp__", 0) == 0)
        {
            thunkByName[symbol.name] = symbol.address;
            thunkAddresses.insert(symbol.address);
        }
    }

    const uint8_t* data = xex.data();
    auto* header = reinterpret_cast<const Xex2ImportHeader*>(getOptHeaderPtr(data, XEX_HEADER_IMPORT_LIBRARIES));
    size_t variables = 0, functions = 0;
    if (header)
    {
        std::vector<std::string> libraries;
        const char* strings = reinterpret_cast<const char*>(header + 1);
        size_t offset = 0;
        for (size_t i = 0; i < header->numImports; i++)
        {
            libraries.emplace_back(strings + offset);
            offset += ((libraries.back().length() + 1) + 3) & ~3;
        }

        auto* library = reinterpret_cast<const Xex2ImportLibrary*>(
            reinterpret_cast<const char*>(header) + sizeof(Xex2ImportHeader) + header->sizeOfStringTable);
        for (size_t i = 0; i < libraries.size(); i++)
        {
            auto* descriptors = reinterpret_cast<const Xex2ImportDescriptor*>(library + 1);
            const auto& names = libraries[i] == "xam.xex" ? XamExports : XboxKernelExports;
            for (size_t im = 0; im < library->numberOfImports; im++)
            {
                const uint32_t address = descriptors[im].firstThunk;
                if (thunkAddresses.count(address)) continue;  // thunk de funcao, ja sobrescrito
                const auto* record = reinterpret_cast<const Xex2ThunkData*>(image.Find(address));
                auto found = names.find(record->originalData.ordinal);
                if (found == names.end()) continue;
                const std::string name = found->second;
                auto thunk = thunkByName.find(name);
                if (thunk != thunkByName.end())
                {
                    imports << std::hex << "func 0x" << address << " 0x" << thunk->second << " " << name << '\n';
                    ++functions;
                }
                else
                {
                    imports << std::hex << "var 0x" << address << " " << name << '\n';
                    ++variables;
                }
            }
            library = reinterpret_cast<const Xex2ImportLibrary*>(
                reinterpret_cast<const char*>(library + 1) + library->numberOfImports * sizeof(Xex2ImportDescriptor));
        }
    }

    std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(image.data.get()), image.size);
    if (!out)
    {
        std::fprintf(stderr, "falha ao gravar %s\n", argv[2]);
        return 1;
    }

    std::printf("base=0x%zX tamanho=0x%X entrada=0x%zX secoes=%zu variaveis=%zu funcoes_na_iat=%zu\n",
        image.base, image.size, image.entry_point, image.sections.size(), variables, functions);
    return 0;
}
