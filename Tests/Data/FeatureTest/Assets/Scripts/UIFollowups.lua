local Gallery = {}
function Gallery:OnUpdate(dt)
    UI.SetFocus(Scene.FindEntityByName("NameInput"))
    local measured = UI.MeasureText("Measured immediate UI", { Size = 28 })
    UI.PushClip(1100, 850, measured.x, 90)
    UI.Rect(1100, 850, measured.x + 100, 90, Vec4(0.2, 0.4, 0.6, 1))
    UI.Text("Measured immediate UI", 1100, 870, { Size = 28, Shadow = false })
    UI.PopClip()
end
return Gallery
