export function cloneDashboardLayout(layout) {
  return typeof structuredClone==='function'?structuredClone(layout):JSON.parse(JSON.stringify(layout));
}

export function upsertDashboardWidget(layout,widget,maxWidgets=20) {
  const index=layout.widgets.findIndex(item=>item.id===widget.id);
  if(index<0&&layout.widgets.length>=maxWidgets)throw new Error(`A dashboard can contain at most ${maxWidgets} widgets.`);
  if(index<0)layout.widgets.push(widget);else layout.widgets[index]=widget;
  return layout;
}

export function removeDashboardWidget(layout,widgetID) {
  layout.widgets=layout.widgets.filter(item=>item.id!==widgetID);return layout;
}

export function moveDashboardWidget(layout,widgetID,offset) {
  const from=layout.widgets.findIndex(item=>item.id===widgetID),to=from+offset;
  if(from<0||to<0||to>=layout.widgets.length)return layout;
  const [widget]=layout.widgets.splice(from,1);layout.widgets.splice(to,0,widget);return layout;
}

export function resizeDashboardWidget(layout,widgetID,size) {
  if(!['small','medium','large'].includes(size))throw new Error('Choose a small, medium, or large dashboard widget size.');
  const widget=layout.widgets.find(item=>item.id===widgetID);if(widget)widget.size=size;return layout;
}
