#CSR = #sparse_tensor.encoding<{
  map = (d0, d1) -> (d1 : dense, d0 : compressed)
}>

func.func @f(%x: tensor<?x?xf32>) -> tensor<?x?xf32>
{
  return %x: tensor<?x?xf32> 
}
