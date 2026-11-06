import test from 'node:test';
import assert from 'node:assert/strict';
import {createContentViews} from '../views/content.js';

function overviewMarkup(state) {
  const views=createContentViews({
    renderObjectControls:()=>'',
    renderObjectList:()=>'',
    renderObjectTable:()=>'',
    renderChecklistView:()=>'',
    renderProgressView:()=>'',
    renderObjectCard:object=>`<span>${object.name}</span>`,
    renderGoals:()=>'',
    renderGoal:()=>'',
    state,
    root:{addEventListener(){},querySelector(){return null;}},
  });
  return views.renderOverview();
}

test('Overview recent objects come only from the game-wide summary',()=>{
  const markup=overviewMarkup({
    overview:{object_count:1200,recent_objects:[{name:'Recently updated'}]},
    overviewLoading:false,
    overviewError:'',
    objects:[{name:'Current section page'}],
    sections:[],
    goals:[],
    game:{name:'Game'},
  });
  assert.match(markup,/OBJECT LIBRARY · 1200/);
  assert.match(markup,/Recently updated/);
  assert.doesNotMatch(markup,/Current section page/);
});

test('Overview distinguishes a truly empty library from a summary error',()=>{
  const empty=overviewMarkup({overview:{object_count:0,recent_objects:[]},overviewLoading:false,overviewError:'',objects:[],sections:[],goals:[],game:{}});
  assert.match(empty,/YOUR LIBRARY IS EMPTY/);
  const failed=overviewMarkup({overview:null,overviewLoading:false,overviewError:'Offline',objects:[],sections:[],goals:[],game:{}});
  assert.match(failed,/RECENT OBJECTS COULD NOT BE LOADED/);
  assert.match(failed,/Try again/);
  assert.doesNotMatch(failed,/YOUR LIBRARY IS EMPTY/);
});
