import test from 'node:test';
import assert from 'node:assert/strict';
import {ApiError, request} from '../api.js';

test('API errors preserve HTTP status, backend code, and message',async t=>{
  const original=globalThis.fetch;
  t.after(()=>{globalThis.fetch=original;});
  globalThis.fetch=async()=>new Response(JSON.stringify({error:{code:'unauthenticated',message:'Sign in again.'}}),{status:401,headers:{'content-type':'application/json'}});
  await assert.rejects(request('/api/auth/me'),error=>error instanceof ApiError&&error.status===401&&error.code==='unauthenticated'&&error.message==='Sign in again.');
  globalThis.fetch=async()=>new Response(JSON.stringify({error:{code:'database_error',message:'Try later.'}}),{status:500,headers:{'content-type':'application/json'}});
  await assert.rejects(request('/api/auth/me'),error=>error instanceof ApiError&&error.status===500&&error.code==='database_error');
});

test('network and malformed JSON responses are distinct structured errors',async t=>{
  const original=globalThis.fetch;
  t.after(()=>{globalThis.fetch=original;});
  globalThis.fetch=async()=>{throw new TypeError('offline');};
  await assert.rejects(request('/api/auth/me'),error=>error instanceof ApiError&&error.status===0&&error.code==='network_error');
  globalThis.fetch=async()=>new Response('{broken',{status:200,headers:{'content-type':'application/json'}});
  await assert.rejects(request('/api/auth/me'),error=>error instanceof ApiError&&error.status===200&&error.code==='invalid_response');
});

test('successful no-content API responses remain null',async t=>{
  const original=globalThis.fetch;
  t.after(()=>{globalThis.fetch=original;});
  globalThis.fetch=async()=>new Response(null,{status:204});
  assert.equal(await request('/api/objects/example',{method:'DELETE'}),null);
});
