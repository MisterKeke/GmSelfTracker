import test from 'node:test';
import assert from 'node:assert/strict';
import {createContentViews} from '../views/content.js';

function historyMarkup(state) {
  const views=createContentViews({
    renderObjectControls:()=>'',renderObjectList:()=>'',renderObjectTable:()=>'',
    renderChecklistView:()=>'',renderProgressView:()=>'',renderObjectCard:()=>'',
    renderGoals:()=>'',renderGoal:()=>'',state,
    root:{addEventListener(){},querySelector(){return null;}},
  });
  return views.renderPanel();
}

function stateForHistory(overrides={}) {
  return {
    panel:'history',historyObject:{id:'object',name:'Example',type_name:'Custom',fields:[{id:'level',name:'Level',field_type:'integer'}]},
    historyBackPanel:'library',historyItems:[],historyFields:[{field_id:'level',field_name:'Level',field_type:'integer'}],
    historyTotal:0,historyError:'',historyLoading:false,historyPage:0,historyChart:[],
    historyFilters:{event_type:'',field_id:'',since:'',until:'',chart_field_id:''},
    ...overrides,
  };
}

test('object history renders escaped timeline changes and filters',()=>{
  const markup=historyMarkup(stateForHistory({
    historyItems:[{id:'e',event_type:'updated',created_at:'2026-11-05T10:00:00.000000000Z',changes:[{field_id:'level',field_name:'Level <safe>',field_type:'integer',before:12,after:18}]}],
    historyTotal:1,
  }));
  assert.match(markup,/OBJECT HISTORY/);
  assert.match(markup,/&lt;safe&gt;/);
  assert.match(markup,/12/);
  assert.match(markup,/18/);
  assert.match(markup,/data-history-filter="since"/);
  assert.match(markup,/data-history-filter="chart_field_id"/);
});

test('numeric progression chart ignores non-numeric historical field values',()=>{
  const chart=historyMarkup(stateForHistory({
    historyFilters:{event_type:'',field_id:'',since:'',until:'',chart_field_id:'level'},
    historyChart:[{created_at:'2026-11-04T10:00:00.000000000Z',changes:[{field_id:'level',field_type:'integer',after:10}]},{created_at:'2026-11-05T10:00:00.000000000Z',changes:[{field_id:'level',field_type:'text',after:'unknown'}]}],
  }));
  assert.match(chart,/<svg[\s\S]*aria-label="Numeric field history chart"/);
  assert.match(chart,/<polyline points=/);
  const noNumeric=historyMarkup(stateForHistory({
    historyFilters:{event_type:'',field_id:'',since:'',until:'',chart_field_id:'level'},
    historyChart:[{created_at:'2026-11-05T10:00:00.000000000Z',changes:[{field_id:'level',field_type:'text',after:'unknown'}]}],
  }));
  assert.match(noNumeric,/No compatible numeric values/);
  assert.doesNotMatch(noNumeric,/<polyline points=/);
});

test('object history has explicit loading, error and empty states',()=>{
  assert.match(historyMarkup(stateForHistory({historyLoading:true})),/Loading object history/);
  assert.match(historyMarkup(stateForHistory({historyError:'Offline'})),/HISTORY COULD NOT BE LOADED/);
  assert.match(historyMarkup(stateForHistory()),/NO MATCHING HISTORY/);
});
