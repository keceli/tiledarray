# Input arguments for ccd
```
MAD_NUM_THREADS=<number_of_threads> mpirun -n <number_of_ranks> ./ccd <input_data_file> <read_or_write> <archive_file_name> <number_of_io_ranks>
```
# Demo

## Serialization
```
|compute-240-03|cc> MAD_NUM_THREADS=1 mpirun -n 4 ./ccd ~/soft/nwx/tiledarray/examples/cc/input write ar.bin 
Reading input... done.
Constructing Fock tensors... done.
Constructing v_ab tensors... done.
Constructing v_aa and v_bb tensors... done.
Calculating t amplitudes...
Iteration 0
Serializing tensors to ar.bin with 1 IO ranks.
 error  = 1.94165454689
 energy = -0.203946171781
Iteration 1
Serializing tensors to ar.bin with 1 IO ranks.
 error  = 0.208467880822
 energy = -0.208912982663
Iteration 2
Serializing tensors to ar.bin with 1 IO ranks.
 error  = 0.0446641747755
 energy = -0.211546519882
CCD energy = -0.211546519882
Done!
```
Note that archives will have a suffix .00001, .00002 etc. depending the number of IO ranks, i.e. there will be `n` files
for `n` IO ranks. 

## Deserialization
```
|compute-240-03|cc> MAD_NUM_THREADS=1 mpirun -n 4 ./ccd ~/soft/nwx/tiledarray/examples/cc/input read ar.bin 
Reading input... done.
Constructing Fock tensors... done.
Constructing v_ab tensors... done.
Constructing v_aa and v_bb tensors... done.
Calculating t amplitudes...
Iteration 0
Deserializing tensors from ar.bin with 1 IO ranks.
 error  = 0.0446641747755
 energy = -0.211546519882
CCD energy = -0.211546519882
Done!
```
Note that you should not give the full name of archive files, only the name without the suffix.

