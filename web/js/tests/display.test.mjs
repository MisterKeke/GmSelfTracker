import test from 'node:test';
import assert from 'node:assert/strict';
import {createObjectViews} from '../components/object-views.js';
import {formatDefaultValue,readDefaultValue} from '../utils/formatting.js';

const field=(id,name='Custom',extra={})=>({id,name,object_type_id:'type',field_type:'decimal',...extra});
function card({primary='',fields=[field('amount')],values={amount:42},section=null,errors={}}={}) {
  const state={types:[{id:'type',primary_display_field_id:primary}],objects:[],panel:'library'};
  return createObjectViews({state}).renderObjectCard({id:'object',object_type_id:'type',type_name:'Custom type',name:'Example',fields,values,formula_errors:errors},section);
}
test('primary display uses only an explicitly configured stable field ID',()=>{
  assert.match(card({primary:'amount'}),/class="object-level">Custom: 42/);
  assert.doesNotMatch(card({fields:[field('amount','Level')]}),/class="object-level"/);
  assert.doesNotMatch(card({primary:'foreign'}),/class="object-level"/);
  assert.match(card({primary:'amount',fields:[field('amount','Renamed')]}),/Renamed: 42/);
});
test('archived and incompatible primary fields fall back without stale values',()=>{
  assert.doesNotMatch(card({primary:'amount',fields:[field('amount','Hidden',{archived:true})]}),/42|class="object-level"/);
  assert.doesNotMatch(card({primary:'amount',fields:[field('amount','Other',{object_type_id:'foreign'})]}),/class="object-level"/);
  assert.match(card({primary:'amount',values:{}}),/Custom: —/);
});
test('section visible fields remain authoritative and formula errors are visible',()=>{
  const section={display_config:{visible_field_ids:['other']}};
  const html=card({primary:'amount',fields:[field('amount'),field('other')],section});
  assert.doesNotMatch(html,/class="object-level"/);
  assert.match(card({primary:'amount',errors:{amount:'division by zero'}}),/⚠ division by zero/);
});
test('object and primary field labels are escaped',()=>{
  assert.match(card({primary:'amount',fields:[field('amount','<script>')]}),/&lt;script&gt;: 42/);
});
test('empty defaults remain empty while false, zero and structured defaults round-trip',()=>{
  assert.equal(formatDefaultValue(null),'');assert.equal(formatDefaultValue(undefined),'');
  for(const [value,kind] of [[0,'decimal'],[false,'boolean'],[{current:3,max:10},'progress'],['Text','text']])assert.deepEqual(readDefaultValue(formatDefaultValue(value),kind),value);
});
