import test from 'node:test';
import assert from 'node:assert/strict';
import { cloneDashboardLayout, upsertDashboardWidget, removeDashboardWidget, moveDashboardWidget, resizeDashboardWidget } from '../utils/dashboard-layout.js';

const widget=(id,size='medium')=>({id,type:'notes',title:id,size,config:{text:id}});

test('dashboard widget drafts add, edit, remove, reorder and resize independently',()=>{
  const saved={version:1,widgets:[widget('a'),widget('b'),widget('c')]};
  const draft=cloneDashboardLayout(saved);
  upsertDashboardWidget(draft,widget('d','small'));
  upsertDashboardWidget(draft,{...widget('b','large'),title:'Edited'});
  moveDashboardWidget(draft,'d',-2);
  resizeDashboardWidget(draft,'a','small');
  removeDashboardWidget(draft,'c');
  assert.deepEqual(draft.widgets.map(item=>item.id),['a','d','b']);
  assert.equal(draft.widgets[0].size,'small');
  assert.equal(draft.widgets[2].title,'Edited');
  assert.deepEqual(saved.widgets.map(item=>item.id),['a','b','c']);
});

test('dashboard widget drafts reject an over-limit add and unsupported size',()=>{
  const layout={version:1,widgets:Array.from({length:2},(_,index)=>widget(String(index)))};
  assert.throws(()=>upsertDashboardWidget(layout,widget('third'),2),/at most 2 widgets/);
  assert.throws(()=>resizeDashboardWidget(layout,'0','extra-large'),/small, medium, or large/);
});

