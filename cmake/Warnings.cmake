# Предупреждения компилятора чинятся, а не глушатся.
#
# Каждый -Wno-* обязан иметь рядом комментарий: почему поставлен, сколько мест
# осталось починить и по какому признаку станет ясно, что пора снять. Сейчас
# таких нет ни одного, и это нормальное состояние — если появится, он должен
# выглядеть исключением, а не привычкой.

function(cork_set_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wold-style-cast
        -Wnon-virtual-dtor
        -Wdouble-promotion
        -Wformat=2
        -Werror
    )
endfunction()
