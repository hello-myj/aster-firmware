# 本地补丁

这个目录里的 LVGL 是**可重新生成的**：`prune_src.sh` 会 `rm -rf src` 再从一份
干净的 v9.5.0 checkout 拷回来。所以任何上游没有的改动都必须同时做三件事，
少一件就会在某次 prune 之后无声消失（而且构建照样成功，不看这段只会觉得
"当初明明是好的"）：

| 机制 | 文件 | 作用 |
|---|---|---|
| 补丁 | `lvgl-aster.patch` | 记录改动本身 |
| 重放 | `prune_src.sh` 第 7 步 | prune 之后自动打回来 |
| 闸门 | **缺**（见下） | 没打上就**响亮地失败** |

第 7 步的回放路径原先指向重构前的 `sdk/vendor/lvgl/`，且补丁自身的路径前缀也是同一处旧
路径，所以 `patch -p1 --forward` 其实从来没打上过、失败被 `|| true` 吞掉；闸门脚本
`check_lvgl_patch.py` 在当前仓库里也不存在（原先那行会以非零码退出，但被脚本末尾的
`echo` 掩盖）。两条已随本次改名修正到 `third_party/lvgl/`，**闸门仍缺**：在补上之前，
prune 之后必须手工确认 `third_party/lvgl/src/core/lv_obj.c` 里仍有 `ASTER-PATCH:` 标记。

---

## 1. `LV_OBJ_FLAG_STATE_TRICKLE` 不传递

**文件**：`src/core/lv_obj.c` — `lv_obj_children_add_state()` / `lv_obj_children_remove_state()`

**上游行为**：`lv_obj_add_state(obj, state)` 只在 `obj` 自己带
`LV_OBJ_FLAG_STATE_TRICKLE` 时才把状态递归给子对象；而
`lv_obj_children_add_state()` 回调的是**公开的** `lv_obj_add_state(child, …)`，
于是那次调用会**重新检查 child 身上的标志**——递归在第一个没有标志的后代处就停了。
`remove` 那条路一模一样。

也就是说这个标志是**逐层**生效的，不是"从这一层起整棵子树"。

**为什么是真问题**（不是设计）：`lv_obj_add_flag(obj, LV_OBJ_FLAG_STATE_TRICKLE)`
的文档语义是"状态变化向下滴流"，而滴流一旦开始就不该在半路停住。真机表现是
launcher 的图标瓦片按下时**只有瓦片本身**变亮，里面的圆角色块和图片纹丝不动
——因为色块没有那个标志，滴流到它这里就断了。当时的绕法是给色块也补一个标志，
但那只是把停止点往下推了一层：再深一层的子对象（图片）又断了，得继续补。
真正的修法是让滴流**穿过**整棵子树。

**改法**：子对象助手不再调公开入口，而是自己套用状态（直接 `update_obj_state`，
与公开入口的写法一致），然后**无条件**递归自己的子对象。标志只在**起点**检查。

```c
if(child) {
    lv_state_t new_state = child->state | state;
    if(child->state != new_state) {
        update_obj_state(child, new_state);
    }
    lv_obj_children_add_state(child, state);   /* 继续往下，不再看标志 */
}
```

状态没变时**不**调 `update_obj_state`——那会多发一次 `LV_EVENT_STATE_CHANGED`
并做一次无谓的样式比较。

**代价**：一次滴流会走完整棵子树，而不是在半路停住。LVGL 的控件树很浅，这点
开销可以忽略；而且一旦标志在起点上是开的，"整棵子树"本来就是想要的语义。

**还有一处同样的问题**，同一个提交里一起修了：`remove` 路径。只修 add 会留下
"按下去能亮、松开不灭"的残留按下态——比不修更难查。
