sudo ./doca_vxlan_fwd -l 0-3 -n 4 -a auxiliary:mlx5_core.sf.4,dv_flow_en=2  -a auxiliary:mlx5_core.sf.5,dv_flow_en=2 -- -l 4