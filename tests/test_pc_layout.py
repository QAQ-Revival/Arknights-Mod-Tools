import os,sys,tempfile,unittest
from pathlib import Path
sys.path.insert(0,str(Path(os.environ.get('QAQR_ARKNIGHTS_TOOLS',Path(__file__).resolve().parents[1]))/'python'))
from pc_layout import current_bundles,requested_bundles,_bundle_index
class Paths(unittest.TestCase):
 def setUp(self):
  base=Path(os.environ.get('QAQR_TEST_TMP',tempfile.gettempdir())).resolve()
  self.temp=tempfile.TemporaryDirectory(dir=base);self.root=Path(self.temp.name)
  assert self.root.resolve().is_relative_to(base)
  self.roots=[self.root/'download',self.root/'installed']
 def tearDown(self):self.temp.cleanup();_bundle_index.cache_clear()
 def file(self,root,relative):
  file=root/relative;file.parent.mkdir(parents=True,exist_ok=True);file.write_bytes(b'test');return file
 def test_generic_suffix_and_download_precedence(self):
  self.file(self.roots[1],'avg/imgs/cg.ab');expected=self.file(self.roots[0],'avg/imgs/cg.ab')
  self.assertEqual(current_bundles(self.roots,set(),['wrapper/imgs/cg.ab']),[('avg/imgs/cg.ab',expected)])
 def test_ambiguous_operator_basename_needs_category(self):
  for category in ('chararts','charpack'):self.file(self.roots[1],category+'/char_001_demo.ab')
  self.assertEqual(requested_bundles(self.roots,['char_001_demo.ab']),[])
  self.assertEqual(requested_bundles(self.roots,['skin/chararts/char_001_demo.ab'])[0][0],'chararts/char_001_demo.ab')
 def test_reject_unsafe_request(self):
  with self.assertRaises(ValueError):requested_bundles(self.roots,['../escape.ab'])
 def test_exact_binding_skips_whole_game_index(self):
  from unittest.mock import patch
  expected=self.file(self.roots[0],'skinpack/char_001_demo.ab')
  self.file(self.roots[1],'skinpack/char_001_demo.ab')
  with patch('pc_layout._bundle_index',side_effect=AssertionError('Whole game scan')):
   self.assertEqual(requested_bundles(self.roots,['skinpack/char_001_demo.ab']),[('skinpack/char_001_demo.ab',expected)])
 def test_export_requires_explicit_valid_selection(self):
  from worker import export_selected
  source=self.root/'source';source.mkdir();output=self.root/'out'
  request={'source':str(source),'output':str(output)}
  plans=[{'id':'one','target':'image.png','bundle':'avg/imgs/cg.ab','resource':'image.png'}]
  payloads={'image.png':('image.png',b'payload','one')}
  with self.assertRaises(ValueError):export_selected(request,plans,payloads,True)
  self.assertFalse(output.exists())
  self.assertEqual(export_selected({**request,'selectedIds':['one']},plans,payloads,True),{'one'})
  self.assertEqual((output/'image.png').read_bytes(),b'payload')
  self.assertTrue((output/'.qaqr-resources.json').is_file())
 def test_material_alpha_merge_and_dimensions(self):
  from worker import texture_image
  from PIL import Image
  from types import SimpleNamespace
  def obj(image):return SimpleNamespace(read=lambda:SimpleNamespace(image=image,m_TextureSettings=SimpleNamespace(m_FilterMode=1)))
  color=Image.new('RGBA',(2,1),(20,30,40,255));alpha=Image.new('RGB',(2,1));alpha.putdata([(17,17,17),(232,232,232)])
  record={'object':obj(color),'alphaObject':obj(alpha),'alphaTransformCompatible':True}
  result=texture_image(record,True)
  self.assertEqual([result.getpixel((x,0))[3] for x in range(2)],[17,232])
  self.assertEqual(result.getpixel((0,0))[:3],(20,30,40))
  record['alphaObject']=obj(Image.new('RGB',(1,1),(55,55,55)))
  self.assertEqual(texture_image(record,True).getpixel((1,0))[3],55)
  self.assertEqual(record['alphaResampling']['filter'],'bilinear')
  record['alphaTransformCompatible']=False
  with self.assertRaises(ValueError):texture_image(record,True)
if __name__=='__main__':unittest.main()
