// Exact finite [0,1] float -> guest Z24 -> float, without float64.
uint ch_depth_pack(float z) {
    precise float scaled=z*16777216.0;
    uint q=uint(scaled);
    if(z>0.5) q--;
    else if(scaled-float(q)>0.5) q++;
    return q;
}
float ch_depth_unpack(uint q) {
    return uintBitsToFloat(floatBitsToUint(float(q)*0.000000059604644775390625)+uint(q!=0u));
}
