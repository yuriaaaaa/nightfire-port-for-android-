// O que o programa de janela do SDL não tem do sistema (platform/file_dialog.h, platform/movie_decoder.h): nenhuma
// escolha de arquivo pelo sistema (no Android o lançador escolhe a imagem do disco antes do jogo e a passa por --disc;
// no Linux, --disc ou [game] disc) e nenhum decodificador de MPEG-2 do sistema (os filmes passam pelo do port,
// platform/mpeg2_decoder.h, como no macOS).

#include "platform/file_dialog.h"
#include "platform/movie_decoder.h"

namespace nf {

std::unique_ptr<Mpeg2Decoder::Backend> open_system_mpeg2_decoder(int, int, double, std::string*, std::string* failure) {
    if (failure != nullptr) *failure = "sem decodificador do sistema no SDL (o do port)";
    return nullptr;
}

struct FilePicker::Impl {};
FilePicker::FilePicker() = default;
FilePicker::~FilePicker() = default;
bool FilePicker::busy() const { return false; }
void FilePicker::start(void*, const FilePickerText&) {}
bool FilePicker::take(std::wstring*) { return false; }

}  // namespace nf
