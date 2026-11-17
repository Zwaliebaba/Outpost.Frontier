//! Vector, matrix and quaternion kernels (work package P1.4): the
//! `3DMathEngine` component of `x86math.dll` (`CppSource/x86math/`,
//! interface `I3DMathEngine` in `CppSource/include/x86math.h`).
//!
//! The types are the project's own (decision T13) so that operation order
//! stays under control. Every kernel keeps the reference's operation order
//! and rounds where the reference rounds.
//!
//! # Precision
//!
//! The reference runs on the x87 unit (`/arch:IA32`, `/fp:precise`); the
//! oracle and the dedicated server set its precision control to 53 bits
//! (decision T11). The kernels therefore compute in `f64`, which is what
//! every x87 `+ - * /` and `FSQRT` does at that setting, and round to `f32`
//! exactly where the reference stores a `float`: at each `FSTP dword` in
//! the assembly and at each assignment, `float` argument and `float` return
//! in the compiled C++. For the operand ranges of these kernels the x87's
//! wider register exponent never matters, so the results are the
//! reference's bit for bit, NaN payloads aside. Nothing here uses
//! trigonometry. The client's main thread runs the reference at 24 bits
//! after Direct3D 8 lowers the control word; the port does not model that.
//!
//! The functions are named after the interface methods. Calls whose output
//! is also an input have `*_assign`, `*_assign_rhs` or `*_in_place`
//! variants where the reference's store order makes the result differ.

mod inv_sqrt;
mod kernels;
mod list;
mod types;
mod x87;

pub use inv_sqrt::{inv_sqrt, sqrt};
pub use kernels::{
    GeneralInverse, MIN_DET, MatrixInverse, det, general_inverse, general_inverse_in_place,
    inverse, inverse_in_place, inverse_rotate, inverse_rotate_in_place, inverse_transform,
    matrix_to_quaternion, mul_matrix, mul_matrix_assign, mul_matrix_assign_rhs, mul_quat,
    mul_quat_assign, mul_quat_assign_rhs, mul_transform, mul_transform_assign,
    mul_transform_assign_rhs, quat_slerp, quaternion_to_matrix, rotate, rotate_in_place, scale,
    transform_matrix, transform_matrix_in_place, transform_quat, transform_xform,
    transform_xform_in_place, transpose_transform, transpose_transform_in_place,
};
pub use list::{
    inverse_transform_list, inverse_transform_list_in_place, transform_list_matrix,
    transform_list_matrix_in_place, transform_list_xform, transform_list_xform_in_place,
    transpose_transform_list, transpose_transform_list_in_place,
};
pub use types::{Matrix, Quaternion, Transform, Vector};
