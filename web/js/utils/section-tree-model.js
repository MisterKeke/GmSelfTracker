export function descendants(sections, id) {
  const ids = new Set([id]);
  const children = new Map();
  for (const section of sections) {
    const list = children.get(section.parent_id) || [];
    list.push(section.id); children.set(section.parent_id, list);
  }
  const pending = [id];
  while (pending.length) for (const child of children.get(pending.pop()) || []) {
    if (!ids.has(child)) { ids.add(child); pending.push(child); }
  }
  return ids;
}

export function sectionRows(sections, expanded) {
  const active = sections.filter(section => !section.archived);
  const ids = new Set(active.map(section => section.id));
  const children = new Map();
  for (const section of active) {
    const parent = ids.has(section.parent_id) ? section.parent_id : null;
    const siblings = children.get(parent) || [];
    siblings.push(section); children.set(parent, siblings);
  }
  for (const siblings of children.values()) siblings.sort((a,b) => a.sort_order-b.sort_order || a.name.localeCompare(b.name) || a.id.localeCompare(b.id));
  const pending = [...(children.get(null) || [])].reverse().map(section => ({section, depth:0}));
  const rows = [], seen = new Set();
  while (pending.length) {
    const row = pending.pop();
    if (seen.has(row.section.id)) continue;
    seen.add(row.section.id);
    const nested = children.get(row.section.id) || [];
    rows.push({...row, hasChildren: nested.length > 0});
    if (expanded.has(row.section.id)) for (const section of [...nested].reverse()) pending.push({section, depth:row.depth+1});
  }
  return rows;
}

