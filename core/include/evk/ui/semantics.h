#pragma once

/**
 * @file semantics.h
 * @brief 无障碍语义树：从 View 树提取的第三棵树，供平台无障碍服务
 *        （TalkBack / VoiceOver / 屏幕朗读）遍历与操作。
 *
 * @details 语义树回答「这个界面对看不见的用户是什么」：每个有内容或可
 * 交互的节点给出朗读文本（label）、角色（role）、可执行动作（actions）
 * 与屏幕矩形（bounds），按 z 序组织成树。它与 Widget/Element/View 三棵
 * 树的关系是「按需派生」：
 *
 *   - **数据源在 View 树**：几何（actualRect，帧内 updateActuals 后新鲜）、
 *     可见性（visible）、交互回调（onClick/onLongPress）；控件内省经
 *     `View::fillSemantics` 回填（TextView 回填文本内容、ButtonView 回填
 *     按钮角色与禁用态、ScrollView 回填滚动动作）；
 *   - **手工标注**：Widget 经 `View::semantics` 字段写入 SemanticsConfig
 *     （优先于内省默认）；控件标注用 `semantics(label, role, child)` 包装
 *     或 Button/ImageWidget 的 semanticsLabel 字段；painter 画进顶点流的
 *     文字对语义树不可见，必须手工标注；
 *   - **生成与下发**：`SemanticsOwner::onFrameBuilt` 挂在 buildFrame 尾部，
 *     DFS 收集 → 与上帧 diff → 有变更经平台通道 `invokePlatform(
 *     "a11y/update", json)` 推全量快照（UI 规模几十节点，全量最简单）；
 *   - **平台交互**：平台侧入向调用 `a11y/enabled`（无障碍开关，关闭时
 *     整条收集链路零开销）与 `a11y/action`（动作下行，格式 "id:action"，
 *     tap 触发对应 View 的点击回调）。
 *
 * 语义节点生成规则：View 满足任一条件即成节点——有手工标注、内省回填
 * 了 label/role、有交互动作。不满足的 View 是「透明节点」，其孩子上提
 * 到最近的语义祖先。`semantics.hidden = true` 的整棵子树不进语义树。
 * id 0 恒为合成根（children 为顶层节点），平台侧从这里开始遍历。
 *
 * 已知限制（后续迭代）：滚动期间 bounds 逐帧变化会持续推送更新；圆角
 * clip 不影响语义矩形（取视图矩形）；scroll 动作暂不上行到平台动作
 * （performAction 仅 tap/longPress）。
 */

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "evk/ui/render_view.h"

namespace evk::ui {

/// 节点角色（对照 Android 的 className / Flutter 的 SemanticsRole 精简集）。
enum class SemanticsRole : int32_t {
    kNone = 0,  ///< 未指定（容器/分组）
    kText,      ///< 静态文本
    kButton,    ///< 按钮
    kImage,     ///< 图片
    kHeader,    ///< 标题
    kList,      ///< 列表
    kScroller,  ///< 可滚动容器
};

/// 可执行动作位（performAction 的参数）。
inline constexpr uint32_t kSemanticsActionTap = 1u << 0;
inline constexpr uint32_t kSemanticsActionLongPress = 1u << 1;
inline constexpr uint32_t kSemanticsActionScrollForward = 1u << 2;
inline constexpr uint32_t kSemanticsActionScrollBackward = 1u << 3;

/// 状态位（enabled 为默认态，禁用时置 kDisabled 位）。
inline constexpr uint32_t kSemanticsStateDisabled = 1u << 0;
inline constexpr uint32_t kSemanticsStateSelected = 1u << 1;

/**
 * @brief 挂在 View 上的手工语义标注（空 = 透明节点）。
 *
 * 与 `View::fillSemantics` 的关系：收集时先由内省回填默认值，再叠加
 * 手工标注（label/hint 非空、role 非 kNone 才覆盖）；hidden 只在手工
 * 标注里设置，置真整棵子树不进语义树。
 */
struct SemanticsConfig {
    std::string label;              ///< 朗读文本（空 = 由内省回填/无朗读内容）
    std::string hint;               ///< 操作提示（如「双击激活」的用途说明）
    SemanticsRole role = SemanticsRole::kNone;
    uint32_t actions = 0;           ///< 内省侧补充的动作位（如 ScrollView）
    bool disabled = false;          ///< 禁用态（ButtonView 回填）
    bool hidden = false;            ///< 整棵子树对无障碍隐藏
};

/// 语义节点（一帧内的不可变快照）。
struct SemanticsNode {
    int32_t id = 0;
    std::string label;
    std::string hint;
    SemanticsRole role = SemanticsRole::kNone;
    uint32_t actions = 0;
    uint32_t state = 0;
    Rect bounds{};                   ///< 屏幕绝对坐标
    std::vector<int32_t> children;   ///< 子节点 id，z 序（绘制序）

    bool operator==(const SemanticsNode& other) const;
    bool operator!=(const SemanticsNode& other) const { return !(*this == other); }
};

/**
 * @brief 语义树的构建者与平台桥（进程单例）。
 *
 * 未启用（平台未开无障碍）时 onFrameBuilt 直接短路，语义路径零开销。
 * 启用后每帧收集并与上帧 diff，有变更推全量快照到平台通道。
 * 视图指针跨帧稳定性：节点 id 经 View* 映射分配，View 析构后按
 * 「本帧未访问即剪枝」回收；performAction 经 ViewRef 做生命周期检查。
 */
class SemanticsOwner {
public:
    static SemanticsOwner& instance();

    SemanticsOwner(const SemanticsOwner&) = delete;
    SemanticsOwner& operator=(const SemanticsOwner&) = delete;

    bool enabled() const { return enabled_; }
    /// 平台无障碍开关上行（a11y/enabled）。关闭时清空快照并停止收集。
    void setEnabled(bool enabled);

    /// 帧构建尾部调用（buildFrame）：收集 → diff → 有变更推 a11y/update。
    void onFrameBuilt();

    /// 当前快照（DFS 序；id 0 为合成根）。
    const std::vector<SemanticsNode>& nodes() const { return nodes_; }
    const SemanticsNode* node(int32_t id) const;
    /// 变更计数：每次 diff 出有实质变化 +1（测试与调试观测用）。
    uint32_t changeCounter() const { return changeCounter_; }

    /// 平台动作下行（a11y/action "id:action"）。返回是否消费。
    bool performAction(int32_t id, uint32_t action);

    /// 复位（关闭、清空快照与 id 映射）。测试与 shutdownApp 路径用。
    void reset();

private:
    SemanticsOwner();

    /// 从 view 递归收集；parentChildren 为上层节点的 children 列表
    /// （透明节点的孩子直接上提追加到这里）。stableIds 为上帧的
    /// View*→id 映射（跨帧复用稳定 id）；本帧存活视图重建进 ids_。
    void collectFrom(View& view,
                     const std::unordered_map<const View*, int32_t>& stableIds,
                     std::vector<SemanticsNode>& out,
                     std::unordered_map<int32_t, ViewRef>& views,
                     std::unordered_map<int32_t, ViewRef>& actionViews,
                     std::vector<int32_t>& parentChildren);

    /// 动作吸收：把「只有动作、无内容/角色/手工标注」的叶子子孙的动作位
    /// 并入 config，动作路由目标记入 target——标注容器包 onTap 容器这类
    /// 常见结构只产生一个节点（被吸收视图经 absorbed_ 在收集中跳过）。
    void absorbLeafActions(View& view, SemanticsConfig& config, ViewRef& target);
    /// 序列化当前快照为 JSON（core 只序列化不解析；解析在各平台侧）。
    std::string serializeJson() const;

    bool enabled_ = false;
    std::vector<SemanticsNode> nodes_;                 ///< 当前帧快照
    std::unordered_map<int32_t, SemanticsNode> previous_;  ///< 上帧（diff 基准）
    std::unordered_map<const View*, int32_t> ids_;     ///< View → 稳定节点 id
    std::unordered_map<int32_t, ViewRef> views_;       ///< 节点 id → View（弱寿命）
    /// 节点 id → 动作实际承载 View（动作吸收的叶子；无则回落 views_）。
    std::unordered_map<int32_t, ViewRef> actionViews_;
    /// 本轮收集中被动作吸收跳过的 View（只在一轮收集内存活）。
    std::unordered_set<const View*> absorbed_;
    int32_t nextId_ = 1;
    uint32_t changeCounter_ = 0;
};

} // namespace evk::ui
