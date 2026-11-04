import test from 'node:test';
import assert from 'node:assert/strict';
import {fieldMatches,queryObjects,FILTER_OPERATORS} from '../utils/object-query.js';
import {descendants,sectionRows} from '../utils/section-tree-model.js';

const object=(id,kind,value,extra={})=>({id,name:id,object_type_id:'type',type_name:'Custom type',description:'Sample description',fields:[{id:'field',field_type:kind}],values:{field:value},...extra});
const match=(kind,value,operator,target,value_to)=>fieldMatches(object('object',kind,value),{field_id:'field',operator,value:target,value_to});

test('numeric comparisons, boundaries, invalid and missing inputs',()=>{
  for(const kind of ['integer','decimal','percentage','formula']) {
    for(const [op,target,want] of [['eq',70,true],['ne',71,true],['gt',70,false],['gte',70,true],['lt',71,true],['lte',70,true]])assert.equal(match(kind,70,op,target),want,`${kind} ${op}`);
    assert.equal(match(kind,70,'between',70,80),true);
    assert.equal(match(kind,70,'between',71,80),false);
    for(const value of [null,undefined,'',NaN,'70'])assert.equal(match(kind,value,'ne',1),false);
  }
});
test('text, choices, boolean, date, progress and reference comparisons',()=>{
  assert.equal(match('text','Pixel World','contains','WORLD'),true);
  assert.equal(match('long_text','Pixel World','not_contains','moon'),true);
  assert.equal(match('text','Pixel World','starts_with','pixel'),true);
  assert.equal(match('text','Pixel World','eq','pixel world'),true);
  assert.equal(match('select','A','one_of',['B','A']),true);
  assert.equal(match('select','A','ne','B'),true);
  assert.equal(match('boolean',false,'unchecked'),true);
  assert.equal(match('boolean',undefined,'unchecked'),false);
  assert.equal(match('date','2026-11-04','before','2026-11-05'),true);
  assert.equal(match('date','2026-11-04','after','2026-11-03'),true);
  assert.equal(match('date','2026-11-04','on','2026-11-04'),true);
  assert.equal(match('date','2026-11-04','between','2026-11-01','2026-11-04'),true);
  assert.equal(match('progress',{current:3,max:4},'above',70),true);
  assert.equal(match('progress',{current:3,max:4},'below',80),true);
  assert.equal(match('progress',{current:4,max:4},'completed'),true);
  assert.equal(match('progress',{current:3,max:4},'not_completed'),true);
  assert.equal(match('progress',{current:0,max:0},'below',50),false);
  assert.equal(match('reference','a','eq','a'),true);
  assert.equal(match('references',['a','b'],'eq','b'),true);
  assert.equal(match('references',[],'ne','b'),true);
});
test('ALL/ANY, search, mixed types, archived fields and formula errors',()=>{
  const objects=[object('Alpha','integer',10),object('Beta','integer',20),object('foreign','integer',30,{object_type_id:'other',fields:[{id:'other-field',field_type:'integer'}]})];
  const conditions=[{field_id:'field',operator:'gte',value:10},{field_id:'field',operator:'lt',value:20}];
  assert.deepEqual(queryObjects(objects,{type_id:'type',conditions,mode:'all'}).map(x=>x.id),['Alpha']);
  assert.deepEqual(queryObjects(objects,{conditions,mode:'any'}).map(x=>x.id),['Alpha','Beta']);
  assert.equal(queryObjects(objects,{search:'custom type'}).length,3);
  assert.equal(queryObjects(objects,{search:'sample description'}).length,3);
  assert.equal(queryObjects(objects,{search:'ALPHA'}).length,1);
  assert.equal(fieldMatches(object('x','integer',30,{fields:[{id:'field',field_type:'integer',archived:true}]}),conditions[0]),false);
  assert.equal(fieldMatches(object('x','formula',30,{formula_errors:{field:'missing input'}}),conditions[0]),false);
});
test('sorting keeps missing last in both directions with stable tie breakers',()=>{
  const objects=[object('b','decimal',10),object('a','decimal',10),object('z','decimal',null),object('c','decimal',2)];
  assert.deepEqual(queryObjects(objects,{sort:'field',direction:'asc'}).map(x=>x.id),['c','a','b','z']);
  assert.deepEqual(queryObjects(objects,{sort:'field',direction:'desc'}).map(x=>x.id),['a','b','c','z']);
  assert.equal(objects[0].id,'b');
  assert.deepEqual(queryObjects(objects,{sort:'name',direction:'desc'}).map(x=>x.id),['z','c','b','a']);
  const dated=[object('a','text','',{created_at:'2026-01-01',updated_at:'2026-03-01'}),object('b','text','',{created_at:'2026-02-01',updated_at:'2026-02-01'})];
  assert.equal(queryObjects(dated,{sort:'created_at',direction:'desc'})[0].id,'b');
  assert.equal(queryObjects(dated,{sort:'updated_at',direction:'desc'})[0].id,'a');
});
test('every custom field kind has a defined filter family',()=>{
  assert.equal(Object.keys(FILTER_OPERATORS).length,12);
});
test('tree nesting, sibling order, collapsed branches and archived ancestors',()=>{
  const sections=[{id:'root',parent_id:null,name:'Root',sort_order:0},{id:'b',parent_id:'root',name:'B',sort_order:2},{id:'a',parent_id:'root',name:'A',sort_order:1},{id:'leaf',parent_id:'a',name:'Leaf',sort_order:0}];
  assert.deepEqual(sectionRows(sections,new Set()).map(x=>x.section.id),['root']);
  assert.deepEqual(sectionRows(sections,new Set(['root','a'])).map(x=>x.section.id),['root','a','leaf','b']);
  assert.deepEqual([...descendants(sections,'a')],['a','leaf']);
  sections[0].archived=true;
  assert.deepEqual(sectionRows(sections,new Set(['a'])).map(x=>[x.section.id,x.depth]),[['a',0],['leaf',1],['b',0]]);
});
test('tree has no arbitrary logical nesting limit',()=>{
  const sections=Array.from({length:300},(_,i)=>({id:String(i),parent_id:i?String(i-1):null,name:String(i),sort_order:0}));
  const rows=sectionRows(sections,new Set(sections.map(x=>x.id)));
  assert.equal(rows.length,300);assert.equal(rows.at(-1).depth,299);
});
