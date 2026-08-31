use std::{cmp::Ordering, collections::HashMap};

#[derive(Clone)]
struct Item {
    uuid: String,
    deadline: u64,
}

#[derive(Clone, Default)]
pub(crate) struct DeadlineQueue {
    items: Vec<Item>,
    indices: HashMap<String, usize>,
}

impl DeadlineQueue {
    /// 创建可预留 `capacity` 个 UUID 的空索引最小堆。
    pub(crate) fn with_capacity(capacity: usize) -> Self {
        Self {
            items: Vec::with_capacity(capacity),
            indices: HashMap::with_capacity(capacity),
        }
    }

    /// 插入或更新 `uuid` 的绝对 `deadline`。
    ///
    /// 已存在项原地调整堆方向，不分配；新项同时写入堆和 O(1) 位置索引。
    pub(crate) fn set(&mut self, uuid: &str, deadline: u64) {
        if let Some(&index) = self.indices.get(uuid) {
            let previous = self.items[index].deadline;
            self.items[index].deadline = deadline;
            match deadline.cmp(&previous) {
                Ordering::Less => self.up(index),
                Ordering::Equal => {}
                Ordering::Greater => {
                    self.down(index);
                }
            }
            return;
        }
        let index = self.items.len();
        self.items.push(Item {
            uuid: uuid.to_owned(),
            deadline,
        });
        self.indices.insert(uuid.to_owned(), index);
        self.up(index);
    }

    /// 删除 `uuid` 并修复堆与位置索引；不存在时返回 false。
    pub(crate) fn remove(&mut self, uuid: &str) -> bool {
        let Some(index) = self.indices.remove(uuid) else {
            return false;
        };
        let last = self.items.len() - 1;
        if index == last {
            self.items.pop();
            return true;
        }
        self.items.swap_remove(index);
        self.indices.insert(self.items[index].uuid.clone(), index);
        if !self.down(index) {
            self.up(index);
        }
        true
    }

    /// 当最早 deadline 小于等于 `now` 时弹出并返回其 UUID；否则返回 `None`。
    pub(crate) fn expire(&mut self, now: u64) -> Option<String> {
        if self.items.first().is_none_or(|item| item.deadline > now) {
            return None;
        }
        let uuid = self.items[0].uuid.clone();
        self.remove(&uuid);
        Some(uuid)
    }

    /// 无条件弹出最早 deadline 的 UUID；空队列返回 `None`。
    pub(crate) fn pop(&mut self) -> Option<String> {
        let uuid = self.items.first()?.uuid.clone();
        self.remove(&uuid);
        Some(uuid)
    }

    /// 返回最早绝对 deadline，但不修改队列。
    pub(crate) fn next(&self) -> Option<u64> {
        self.items.first().map(|item| item.deadline)
    }

    /// 从 `index` 向父节点上浮，恢复 `(deadline, uuid)` 确定顺序的最小堆性质。
    fn up(&mut self, mut index: usize) {
        while index > 0 {
            let parent = (index - 1) / 2;
            if !self.less(index, parent) {
                break;
            }
            self.swap(index, parent);
            index = parent;
        }
    }

    /// 从 `index` 向最小子节点下沉并恢复堆性质；返回该项是否移动。
    fn down(&mut self, mut index: usize) -> bool {
        let start = index;
        loop {
            let left = index * 2 + 1;
            if left >= self.items.len() {
                return index != start;
            }
            let right = left + 1;
            let child = if right < self.items.len() && self.less(right, left) { right } else { left };
            if !self.less(child, index) {
                return index != start;
            }
            self.swap(index, child);
            index = child;
        }
    }

    /// 比较 `left` 与 `right` 堆项；deadline 相等时用 UUID 提供确定顺序。
    fn less(&self, left: usize, right: usize) -> bool {
        (self.items[left].deadline, &self.items[left].uuid) < (self.items[right].deadline, &self.items[right].uuid)
    }

    /// 交换两个堆位置，并同步更新两个 UUID 的位置索引。
    fn swap(&mut self, left: usize, right: usize) {
        self.items.swap(left, right);
        self.indices.insert(self.items[left].uuid.clone(), left);
        self.indices.insert(self.items[right].uuid.clone(), right);
    }
}

#[cfg(test)]
#[path = "../../tests/internal/registration/deadline.rs"]
mod tests;
