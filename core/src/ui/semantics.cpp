/**
 * @file semantics.cpp
 * @brief 无障碍语义树的收集、diff 与平台桥（设计说明见头文件）。
 *
 * @details 每帧流程（onFrameBuilt，仅 enabled_ 时执行）：
 *   1. DFS View 树收集：内省（fillSemantics）→ 手工标注（View::semantics）
 *      覆盖 → 通用动作推导（onClick/onLongPress）→ 动作吸收（只有动作
 *      的叶子子孙并入本节点，动作路由到被吸收 View）。满足「有朗读内容、
 *      有角色或有动作」的 View 成节点，否则透明、孩子上提；
 *   2. id 分配：跨帧按 View* 复用稳定 id；上帧映射整体换出，本轮只把
 *      存活视图重建进 ids_——View 析构不主动通知，靠这里剪枝（地址被
 *      复用最多继承旧 id，diff 会按内容修正）；
 *   3. 与上帧逐节点 diff：有增删改则 changeCounter_ +1 并经平台通道推
 *      全量快照（UI 规模几十节点，全量比增量协议简单得多）。
 *
 * 动作下行（a11y/action，"id:action"）：节点声明了该动作才执行；动作
 * 优先落到被吸收的动作承载 View，回落节点自身 View，经 ViewRef 判活
 * 后调 View::performSemanticsAction——回调与触摸同路（tap→onClick/
 * onPressed），回调内 setState 重建与触摸场景同构。
 */

#include "evk/ui/semantics.h"

#include <cstdio>
#include <utility>

#include "evk/frame_scheduler.h"
#include "evk/platform_channel.h"

namespace evk::ui {

namespace {

bool rectEquals(const Rect& a, const Rect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

/// JSON 字符串转义（label/hint 可能含引号/反斜杠/控制字符）。
void appendJsonEscaped(std::string& out, const std::string& text) {
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
}

void appendJsonString(std::string& out, const char* key, const std::string& value) {
    out += ",\"";
    out += key;
    out += "\":\"";
    appendJsonEscaped(out, value);
    out += '"';
}

} // namespace

bool SemanticsNode::operator==(const SemanticsNode& other) const {
    return id == other.id && label == other.label && hint == other.hint &&
           role == other.role && actions == other.actions && state == other.state &&
           rectEquals(bounds, other.bounds) && children == other.children;
}

SemanticsOwner& SemanticsOwner::instance() {
    static SemanticsOwner owner;
    return owner;
}

SemanticsOwner::SemanticsOwner() {
    // 平台入向：无障碍开关（TalkBack/VoiceOver 启停）。
    setPlatformHandler("a11y/enabled", [](const std::string& args) {
        SemanticsOwner::instance().setEnabled(args == "1" || args == "true");
        return std::string("1");
    });
    // 平台入向：动作下行，格式 "id:action"（core 不解析 JSON，平台侧配合）。
    setPlatformHandler("a11y/action", [](const std::string& args) {
        int id = 0;
        unsigned action = 0;
        if (std::sscanf(args.c_str(), "%d:%u", &id, &action) != 2) {
            return std::string();
        }
        return SemanticsOwner::instance().performAction(id, action)
                   ? std::string("1")
                   : std::string("0");
    });
}

void SemanticsOwner::setEnabled(bool enabled) {
    if (enabled_ == enabled) {
        return;
    }
    enabled_ = enabled;
    if (!enabled_) {
        // 关闭：清空本地快照并推一次空树，平台侧同步清空缓存。
        nodes_.clear();
        previous_.clear();
        views_.clear();
        actionViews_.clear();
        ids_.clear();
        nextId_ = 1;
        ++changeCounter_;
        invokePlatform("a11y/update", "{\"change\":-1,\"nodes\":[]}");
        return;
    }
    // 启用：强制一帧，onFrameBuilt 全量收集并推送。
    requestRender();
}

void SemanticsOwner::reset() {
    enabled_ = false;
    nodes_.clear();
    previous_.clear();
    views_.clear();
    actionViews_.clear();
    ids_.clear();
    absorbed_.clear();
    nextId_ = 1;
    changeCounter_ = 0;
}

void SemanticsOwner::onFrameBuilt() {
    if (!enabled_) {
        return;
    }
    View* root = rootView();
    if (!root) {
        return;
    }

    // 上帧映射换出作稳定 id 来源；ids_ 本轮重建为「存活视图」集合。
    std::unordered_map<const View*, int32_t> stableIds;
    stableIds.swap(ids_);
    absorbed_.clear();

    std::vector<SemanticsNode> fresh;
    std::unordered_map<int32_t, ViewRef> freshViews;
    std::unordered_map<int32_t, ViewRef> freshActionViews;

    // 合成根 id 0：平台遍历的单一入口，children 为顶层语义节点。
    SemanticsNode rootNode;
    rootNode.id = 0;
    rootNode.bounds = root->actualRect();
    fresh.push_back(std::move(rootNode));

    // 顶层孩子先收进本地列表，收集完成再回填：fresh 在递归中扩容，
    // 持有元素引用会失效，按下标回填才安全（根节点恒在下标 0）。
    std::vector<int32_t> topLevel;
    collectFrom(*root, stableIds, fresh, freshViews, freshActionViews, topLevel);
    fresh[0].children = std::move(topLevel);

    // diff：与上帧逐节点对比（增/删/改任一即变更）。
    bool changed = fresh.size() != previous_.size();
    if (!changed) {
        std::unordered_map<int32_t, const SemanticsNode*> freshById;
        freshById.reserve(fresh.size());
        for (const SemanticsNode& node : fresh) {
            freshById.emplace(node.id, &node);
        }
        for (const auto& entry : previous_) {
            auto it = freshById.find(entry.first);
            if (it == freshById.end() || *it->second != entry.second) {
                changed = true;
                break;
            }
        }
    }
    if (!changed) {
        return;
    }

    ++changeCounter_;
    nodes_ = std::move(fresh);
    views_ = std::move(freshViews);
    actionViews_ = std::move(freshActionViews);
    previous_.clear();
    previous_.reserve(nodes_.size());
    for (const SemanticsNode& node : nodes_) {
        previous_.emplace(node.id, node);
    }
    invokePlatform("a11y/update", serializeJson());
}

void SemanticsOwner::collectFrom(
    View& view,
    const std::unordered_map<const View*, int32_t>& stableIds,
    std::vector<SemanticsNode>& out,
    std::unordered_map<int32_t, ViewRef>& views,
    std::unordered_map<int32_t, ViewRef>& actionViews,
    std::vector<int32_t>& parentChildren) {
    if (!view.visible || absorbed_.count(&view) != 0) {
        return;
    }
    if (view.semantics && view.semantics->hidden) {
        return;  // 整棵子树对无障碍隐藏
    }

    // 内省默认 → 手工标注覆盖 → 通用动作推导。
    SemanticsConfig config;
    view.fillSemantics(config);
    if (view.semantics) {
        const SemanticsConfig& manual = *view.semantics;
        if (!manual.label.empty()) config.label = manual.label;
        if (!manual.hint.empty()) config.hint = manual.hint;
        if (manual.role != SemanticsRole::kNone) config.role = manual.role;
        config.actions |= manual.actions;
        config.disabled = config.disabled || manual.disabled;
    }
    if (view.onClick) config.actions |= kSemanticsActionTap;
    if (view.onLongPress) config.actions |= kSemanticsActionLongPress;

    const bool worthy = !config.label.empty() ||
                        config.role != SemanticsRole::kNone ||
                        config.actions != 0;
    if (!worthy) {
        // 透明节点：孩子直接上提到父节点的孩子列表。
        for (const auto& child : view.children) {
            if (child) {
                collectFrom(*child, stableIds, out, views, actionViews,
                            parentChildren);
            }
        }
        return;
    }

    // 动作吸收：「只有动作的叶子子孙」并入本节点（标注容器包 onTap
    // 容器只产生一个节点，动作路由到被吸收的 View）。
    ViewRef actionTarget;
    absorbLeafActions(view, config, actionTarget);

    auto it = stableIds.find(&view);
    const int32_t id = it != stableIds.end() ? it->second : nextId_++;
    SemanticsNode node;
    node.id = id;
    node.label = std::move(config.label);
    node.hint = std::move(config.hint);
    node.role = config.role;
    node.actions = config.actions;
    if (config.disabled) node.state |= kSemanticsStateDisabled;
    node.bounds = view.actualRect();
    parentChildren.push_back(id);
    out.push_back(std::move(node));
    views.emplace(id, view.ref());
    ids_.emplace(&view, id);  // 记入本帧存活集合（跨帧稳定复用）
    if (actionTarget) {
        actionViews.emplace(id, actionTarget);
    }
    const size_t nodeIndex = out.size() - 1;

    // 孩子 id 先收进本地列表再按下标回填（out 递归中会扩容）。
    std::vector<int32_t> kids;
    for (const auto& child : view.children) {
        if (child) {
            collectFrom(*child, stableIds, out, views, actionViews, kids);
        }
    }
    out[nodeIndex].children = std::move(kids);
}

void SemanticsOwner::absorbLeafActions(
    View& view,
    SemanticsConfig& config,
    ViewRef& target) {
    for (const auto& child : view.children) {
        if (!child || !child->visible || absorbed_.count(child.get()) != 0) {
            continue;
        }
        if (child->semantics) {
            continue;  // 手工标注的子孙独立成节点，不吸收
        }
        SemanticsConfig childConfig;
        child->fillSemantics(childConfig);
        if (!childConfig.label.empty() ||
            childConfig.role != SemanticsRole::kNone) {
            continue;  // 有内省内容/角色的子孙（文本/按钮/滚动容器）独立成节点
        }
        uint32_t childActions = childConfig.actions;
        if (child->onClick) childActions |= kSemanticsActionTap;
        if (child->onLongPress) childActions |= kSemanticsActionLongPress;
        if (childActions == 0) {
            absorbLeafActions(*child, config, target);  // 透明层继续向下看
            continue;
        }
        if (!child->children.empty()) {
            continue;  // 只吸收叶子，避免吞掉嵌套内容
        }
        config.actions |= childActions;
        if (!target) {
            target = child->ref();  // 首个被吸收者承担动作路由
        }
        absorbed_.insert(child.get());
    }
}

const SemanticsNode* SemanticsOwner::node(int32_t id) const {
    for (const SemanticsNode& node : nodes_) {
        if (node.id == id) {
            return &node;
        }
    }
    return nullptr;
}

bool SemanticsOwner::performAction(int32_t id, uint32_t action) {
    const SemanticsNode* target = node(id);
    if (!target || action == 0) {
        return false;
    }
    if ((target->actions & action) != action) {
        return false;  // 节点未声明该动作
    }
    // 动作优先落到被吸收的动作承载 View，回落节点自身 View。
    View* view = nullptr;
    auto actionIt = actionViews_.find(id);
    if (actionIt != actionViews_.end()) {
        view = actionIt->second.get();
    }
    if (!view) {
        auto it = views_.find(id);
        if (it != views_.end()) {
            view = it->second.get();
        }
    }
    if (!view) {
        return false;  // 帧间窗口：View 已销毁，本次动作丢弃
    }
    return view->performSemanticsAction(action);
}

std::string SemanticsOwner::serializeJson() const {
    std::string out;
    out.reserve(1024);
    out += "{\"change\":";
    out += std::to_string(changeCounter_);
    out += ",\"nodes\":[";
    bool first = true;
    for (const SemanticsNode& node : nodes_) {
        if (!first) {
            out += ',';
        }
        first = false;
        out += "{\"id\":";
        out += std::to_string(node.id);
        appendJsonString(out, "label", node.label);
        appendJsonString(out, "hint", node.hint);
        out += ",\"role\":";
        out += std::to_string(static_cast<int32_t>(node.role));
        out += ",\"actions\":";
        out += std::to_string(node.actions);
        out += ",\"state\":";
        out += std::to_string(node.state);
        char rect[96];
        std::snprintf(rect, sizeof(rect), ",\"rect\":[%g,%g,%g,%g]",
                      node.bounds.x, node.bounds.y, node.bounds.w, node.bounds.h);
        out += rect;
        out += ",\"children\":[";
        bool firstChild = true;
        for (const int32_t child : node.children) {
            if (!firstChild) {
                out += ',';
            }
            firstChild = false;
            out += std::to_string(child);
        }
        out += "]}";
    }
    out += "]}";
    return out;
}

} // namespace evk::ui
