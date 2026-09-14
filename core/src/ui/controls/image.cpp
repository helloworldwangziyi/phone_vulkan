#include "evk/ui/controls/image.h"

#include <memory>

#include "evk/ui/render_view.h"
#include "evk/ui/semantics.h"

namespace evk::ui {

ImageWidget::ImageWidget(TextureId texture) : texture_(texture) {}

std::unique_ptr<View> ImageWidget::createRenderObject() const {
    auto view = std::make_unique<View>();
    updateRenderObject(*view);
    return view;
}

void ImageWidget::updateRenderObject(View& view) const {
    view.clearBackground();
    view.painter = [texture = texture_](PaintContext& paint) {
        const Size size = paint.size();
        paint.drawImage(texture, {0.0f, 0.0f, size.width, size.height});
    };
    if (!semanticsLabel.empty()) {
        if (!view.semantics) {
            view.semantics = std::make_unique<SemanticsConfig>();
        }
        view.semantics->label = semanticsLabel;
        view.semantics->role = SemanticsRole::kImage;
    }
}

std::unique_ptr<Widget> image(TextureId texture) {
    return makeWidget<ImageWidget>(texture);
}

} // namespace evk::ui
