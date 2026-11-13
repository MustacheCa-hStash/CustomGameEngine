# Future considerations

## Rendering

- **Normal offset bias for shadows.** Evaluate offsetting the receiver's shadow lookup position along its surface normal, with an offset related to the light angle and shadow-map texel size. This may reduce self-shadowing artifacts while retaining contact shadows. Compare it across sun angles on the cube, bark, and both sides of foliage cards, and watch for detached or distorted shadows.
